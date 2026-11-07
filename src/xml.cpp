#include "xml.hpp"

#include <cctype>
#include <cstdint>
#include <string_view>

namespace xml {
namespace {

constexpr int kMaxDepth = 64;

bool is_name_start(unsigned char c) { return std::isalpha(c) || c == '_' || c == ':' || c >= 0x80; }
bool is_name_char(unsigned char c) {
    return is_name_start(c) || std::isdigit(c) || c == '-' || c == '.';
}

void append_utf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

class Parser {
public:
    explicit Parser(const std::string& s) : s_(s) {}

    std::unique_ptr<Element> parse_document(std::string& error) {
        if (s_.compare(0, 3, "\xEF\xBB\xBF") == 0) i_ = 3;  // UTF-8 BOM
        std::unique_ptr<Element> root;
        if (skip_misc()) {
            if (starts("<!DOCTYPE")) {
                fail("DOCTYPE is not allowed");
            } else if (eof() || s_[i_] != '<') {
                fail("expected root element");
            } else {
                root = parse_element(0, nullptr);
                if (root && skip_misc() && !eof()) fail("unexpected content after root element");
            }
        }
        if (!err_.empty()) {
            error = err_;
            return nullptr;
        }
        return root;
    }

private:
    bool eof() const { return i_ >= s_.size(); }
    bool starts(const char* lit) const { return s_.compare(i_, std::char_traits<char>::length(lit), lit) == 0; }

    void skip_ws() {
        while (!eof() && std::isspace(static_cast<unsigned char>(s_[i_]))) ++i_;
    }

    bool fail(const std::string& msg) {
        if (err_.empty()) err_ = msg + " at offset " + std::to_string(i_);
        return false;
    }

    bool skip_until(const char* terminator) {
        const std::size_t end = s_.find(terminator, i_);
        if (end == std::string::npos) return fail(std::string("missing '") + terminator + "'");
        i_ = end + std::char_traits<char>::length(terminator);
        return true;
    }

    // Whitespace, comments and processing instructions (incl. <?xml ...?>).
    bool skip_misc() {
        for (;;) {
            skip_ws();
            if (starts("<?")) {
                if (!skip_until("?>")) return false;
            } else if (starts("<!--")) {
                if (!skip_until("-->")) return false;
            } else {
                return true;
            }
        }
    }

    bool parse_name(std::string& out) {
        if (eof() || !is_name_start(static_cast<unsigned char>(s_[i_]))) return fail("expected a name");
        const std::size_t start = i_;
        while (!eof() && is_name_char(static_cast<unsigned char>(s_[i_]))) ++i_;
        out = s_.substr(start, i_ - start);
        return true;
    }

    bool decode(std::string_view raw, std::string& out) {
        for (std::size_t j = 0; j < raw.size(); ++j) {
            if (raw[j] != '&') {
                out += raw[j];
                continue;
            }
            const std::size_t semi = raw.find(';', j);
            if (semi == std::string_view::npos) return fail("unterminated entity");
            const std::string_view ent = raw.substr(j + 1, semi - j - 1);
            if (ent == "lt") out += '<';
            else if (ent == "gt") out += '>';
            else if (ent == "amp") out += '&';
            else if (ent == "quot") out += '"';
            else if (ent == "apos") out += '\'';
            else if (ent.size() > 1 && ent[0] == '#') {
                const bool hex = ent[1] == 'x';
                const std::string_view digits = ent.substr(hex ? 2 : 1);
                if (digits.empty() || digits.size() > 8) return fail("bad character reference");
                std::uint32_t cp = 0;
                for (char c : digits) {
                    int v;
                    if (c >= '0' && c <= '9') v = c - '0';
                    else if (hex && c >= 'a' && c <= 'f') v = c - 'a' + 10;
                    else if (hex && c >= 'A' && c <= 'F') v = c - 'A' + 10;
                    else return fail("bad character reference");
                    cp = cp * (hex ? 16 : 10) + static_cast<std::uint32_t>(v);
                }
                if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
                    return fail("invalid code point");
                append_utf8(out, cp);
            } else {
                return fail("unknown entity '&" + std::string(ent) + ";'");
            }
            j = semi;
        }
        return true;
    }

    std::unique_ptr<Element> parse_element(int depth, const Element* parent) {
        if (depth > kMaxDepth) {
            fail("nesting too deep");
            return nullptr;
        }
        auto el = std::make_unique<Element>();
        el->parent = parent;
        ++i_;  // '<'
        if (!parse_name(el->name)) return nullptr;

        // Attributes
        for (;;) {
            const std::size_t before = i_;
            skip_ws();
            if (eof()) {
                fail("unterminated start tag");
                return nullptr;
            }
            if (starts("/>")) {
                i_ += 2;
                return el;
            }
            if (s_[i_] == '>') {
                ++i_;
                break;
            }
            if (i_ == before) {
                fail("expected whitespace before attribute");
                return nullptr;
            }
            std::string attr;
            if (!parse_name(attr)) return nullptr;
            skip_ws();
            if (eof() || s_[i_] != '=') {
                fail("expected '=' after attribute name");
                return nullptr;
            }
            ++i_;
            skip_ws();
            if (eof() || (s_[i_] != '"' && s_[i_] != '\'')) {
                fail("expected quoted attribute value");
                return nullptr;
            }
            const char quote = s_[i_++];
            const std::size_t end = s_.find(quote, i_);
            if (end == std::string::npos) {
                fail("unterminated attribute value");
                return nullptr;
            }
            const std::string_view raw(s_.data() + i_, end - i_);
            if (raw.find('<') != std::string_view::npos) {
                fail("'<' in attribute value");
                return nullptr;
            }
            std::string value;
            if (!decode(raw, value)) return nullptr;
            if (!el->attributes.emplace(attr, std::move(value)).second) {
                fail("duplicate attribute '" + attr + "'");
                return nullptr;
            }
            i_ = end + 1;
        }

        // Content
        for (;;) {
            if (eof()) {
                fail("unclosed element <" + el->name + ">");
                return nullptr;
            }
            if (starts("</")) {
                i_ += 2;
                std::string closing;
                if (!parse_name(closing)) return nullptr;
                if (closing != el->name) {
                    fail("mismatched closing tag </" + closing + "> for <" + el->name + ">");
                    return nullptr;
                }
                skip_ws();
                if (eof() || s_[i_] != '>') {
                    fail("expected '>'");
                    return nullptr;
                }
                ++i_;
                return el;
            }
            if (starts("<!--")) {
                if (!skip_until("-->")) return nullptr;
            } else if (starts("<![CDATA[")) {
                const std::size_t end = s_.find("]]>", i_ + 9);
                if (end == std::string::npos) {
                    fail("unterminated CDATA section");
                    return nullptr;
                }
                el->text.append(s_, i_ + 9, end - i_ - 9);
                i_ = end + 3;
            } else if (starts("<?")) {
                if (!skip_until("?>")) return nullptr;
            } else if (starts("<!")) {
                fail("unexpected markup declaration");
                return nullptr;
            } else if (s_[i_] == '<') {
                auto child = parse_element(depth + 1, el.get());
                if (!child) return nullptr;
                el->children.push_back(std::move(child));
            } else {
                const std::size_t end = s_.find('<', i_);
                if (end == std::string::npos) {
                    fail("unclosed element <" + el->name + ">");
                    return nullptr;
                }
                if (!decode(std::string_view(s_.data() + i_, end - i_), el->text)) return nullptr;
                i_ = end;
            }
        }
    }

    const std::string& s_;
    std::size_t i_ = 0;
    std::string err_;
};

}  // namespace

std::string Element::prefix() const {
    const std::size_t colon = name.find(':');
    return colon == std::string::npos ? std::string() : name.substr(0, colon);
}

std::string Element::local_name() const {
    const std::size_t colon = name.find(':');
    return colon == std::string::npos ? name : name.substr(colon + 1);
}

std::string Element::namespace_uri() const {
    const std::string p = prefix();
    const std::string attr = p.empty() ? "xmlns" : "xmlns:" + p;
    for (const Element* e = this; e != nullptr; e = e->parent) {
        auto it = e->attributes.find(attr);
        if (it != e->attributes.end()) return it->second;
    }
    return "";
}

const Element* Element::child(const std::string& local) const {
    for (const auto& c : children)
        if (c->local_name() == local) return c.get();
    return nullptr;
}

std::string Element::child_text(const std::string& local) const {
    const Element* c = child(local);
    return c ? c->text : std::string();
}

const Element* Element::first_child_element() const {
    return children.empty() ? nullptr : children.front().get();
}

std::unique_ptr<Element> parse(const std::string& input, std::string& error) {
    return Parser(input).parse_document(error);
}

std::string escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default: out += c;
        }
    }
    return out;
}

}  // namespace xml
