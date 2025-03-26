#include <iostream>
#include <restclient-cpp/restclient.h>

int main() {
    RestClient::Response r = RestClient::get("https://httpbin.org/get");

    std::cout << "Status Code: " << r.code << "\n";
    std::cout << "Response Body:\n" << r.body << "\n";

    return 0;
}