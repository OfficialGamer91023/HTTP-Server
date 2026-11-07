// A small, strict XML parser: enough for SOAP envelopes, no external deps.
//
// Supports elements, attributes, text, the five predefined entities, numeric
// character references, CDATA, comments and processing instructions.
// DOCTYPE is rejected outright, which rules out entity-expansion attacks
// ("billion laughs") and external entities (XXE).
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace xml {

struct Element {
    std::string name;  // qualified name as written, e.g. "soap:Envelope"
    std::map<std::string, std::string> attributes;
    std::string text;  // concatenated direct text content
    std::vector<std::unique_ptr<Element>> children;
    const Element* parent = nullptr;

    std::string prefix() const;      // "soap" for "soap:Envelope", "" if none
    std::string local_name() const;  // "Envelope"
    // Resolves this element's prefix through xmlns declarations on it and its ancestors.
    std::string namespace_uri() const;

    const Element* child(const std::string& local) const;  // first child with that local name
    std::string child_text(const std::string& local) const;
    const Element* first_child_element() const;
};

// Returns the root element, or nullptr with `error` set.
std::unique_ptr<Element> parse(const std::string& input, std::string& error);

// Escapes text for use in element content or attribute values.
std::string escape(const std::string& s);

}  // namespace xml
