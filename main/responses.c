#include "responses.h"

const char* INDEX_PAGE_RESPONSE =
    "HTTP/1.1 200 OK\r\n"
    "Content-Length: 50\r\n"
    "Content-Type: text/html\r\n"
    "\r\n"
    "<button onclick=\"fetch('/toggle')\">Toggle</button>";

const char* TOGGLE_RESPONSE =
    "HTTP/1.1 200\r\n"
    "Content-Length: 0\r\n"
    "\r\n";