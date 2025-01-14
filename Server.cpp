#include <iostream>
#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string>
#include <string.h>
#include <unistd.h>
#include <fstream>

using namespace std;

char *parseFT(char path[]);
string getFileContent(string, string);

int main() {
    int fdSocket, fdChildSocket, port, pid;
    sockaddr_in parent, child;
    char message[30000] = {0};
    socklen_t addrlen = sizeof(parent);
    char reponseHeader[] = "HTTP/1.1 200 OK\r\n";
    char reponseHeaderFound[] = "HTTP/1.1 308 Permanent Redirect\r\n";
    char contentType[] = "Content-Type: text/html;charset=UTF-8\r\n";
    char contentTypecss[] = "Content-Type: text/css\r\n";
    char contentTypephp[] = "Content-Type: application/php\r\n";
    char loca[] = "Location: BloodBank/admin_dashboard.php";
    port = 8080;
    char del[2] = " ";
    char delF[2] = "/";

    if ((fdSocket = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        cout << "Failed to create a socket" << endl;
    }
    parent.sin_port = htons(port);
    parent.sin_family = AF_INET;
    parent.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if ((bind(fdSocket, (struct sockaddr *)&parent, sizeof(parent))) < 0) {
        perror("bind");
        exit(EXIT_FAILURE);
    }
    if (listen(fdSocket, 5) < 0) {
        perror("listen");
        exit(EXIT_FAILURE);
    }
    cout << "Listening for connections on port " << port << endl;

    while (true) {
        fdChildSocket = accept(fdSocket, (struct sockaddr *)&parent, &addrlen);
        cout << "Request accepted" << endl;
        if (fdChildSocket < 0) {
            perror("child socket");
            exit(EXIT_FAILURE);
        }
        
        ssize_t bytesread = read(fdChildSocket, message, sizeof(message) - 1);
        if (bytesread == 0) {
            cout << "Tried to read from empty buffer" << endl;
            continue;
        }
        
        string mess(message);
        if (bytesread < 0) {
            perror("read");
            exit(EXIT_FAILURE);
        }

        char *method = strtok(message, del);
        char *filePath = strtok(NULL, del);
        filePath = strtok(filePath, delF);
        string fp(filePath);
        char *fileType = parseFT(filePath);
        string ft(fileType);

        if (method[0] == 'G' && method[1] == 'E' && method[2] == 'T') {
            // Handle GET request
            char response[80000] = {0};
            char length[50] = {0};
            string htmlContent = getFileContent(fp, ft);
            snprintf(length, sizeof(length), "Content-Length: %zu\r\n", htmlContent.length());
            strcat(response, reponseHeader);

            if (ft == "css") {
                strcat(response, contentTypecss);
            } else if (ft == "html") {
                strcat(response, contentType);
            }

            strcat(response, length);
            strcat(response, "\r\n");
            strcat(response, htmlContent.c_str());

            cout << "GET request handled" << endl;
            send(fdChildSocket, response, strlen(response), 0);

        } else if (method[0] == 'P' && method[1] == 'O' && method[2] == 'S' && method[3] == 'T') {
            // Handle POST request
            char response[80000] = {0};
            strcat(response, reponseHeaderFound);
            strcat(response, loca);

            // Extract POST data
            char *postData = strstr(message, "\r\n\r\n");
            if (postData) {
                postData += 4; // Move past the \r\n\r\n
                // Execute PHP script with POST data
                FILE *phpProcess = popen(("SCRIPT_FILENAME=" + fp + " REQUEST_METHOD=POST REDIRECT_STATUS=CGI cgi-fcgi -bind -connect -d / -s /var/run/php/php7.4-fpm.sock").c_str(), "w");
                if (phpProcess) {
                    // Send POST data to PHP process
                    fwrite(postData, sizeof(char), strlen(postData), phpProcess);
                    pclose(phpProcess);

                    // Send response
                    cout << "POST request handled" << endl;
                    send(fdChildSocket, response, strlen(response), 0);
                } else {
                    perror("php-cgi");
                    exit(EXIT_FAILURE);
                }
            } else {
                // Handle no POST data scenario
                cout << "No POST data found" << endl;
                // Send appropriate error response
                send(fdChildSocket, response, strlen(response), 0);
            }
        }

        close(fdChildSocket);
        message[30000] = {0};
    }

    close(fdSocket);
}

char *parseFT(char path[]) {
    char del[2] = ".";
    char *fileType = strtok(path, del);
    char *last;

    while (fileType != NULL) {
        last = fileType;
        fileType = strtok(NULL, del);
    }

    return last;
}

string getFileContent(string path, string ft) {
    if (ft == "jpg" || ft == "png" || ft == "ico") {
        return "";
    }

    string p = "BloodBank/";
    p += path;
    cout << p << endl;
    ifstream myfile(p);

    if (!myfile.is_open()) {
        perror("file");
        exit(EXIT_FAILURE);
        return "";
    }

    string line;
    string message;

    while (getline(myfile, line)) {
        message += line + "\n";
    }

    myfile.close();
    return message;
}
