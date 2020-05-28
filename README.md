# FTPServer and FTPClient
Simple FTP Server and Client for the esp8266/esp32 with LittleFS and SPIFFS support.

I've modified a FTP Server from arduino/wifi shield to work with the esp826. It should also work on the esp32. LittleFS with directory handling is supported since SPIFFS has been marked deprecated.
This allows you to FTP into your esp8266/esp32 and access/modify the LittleFS/SPIFFS folder/data.
I've tested it with Filezilla, and it works (upload/download/rename/delete). There's no create/modify directory support in SPIFFS but in LittleFS there is!

The FTP Client is pretty much straihgt forward. It can upload (put, STOR) a file to a FTP Server or download (get, RETR) a file from a FTP Server. Both ways can be done blocking or non-blocking.

## Features
* Server supports both active and passive mode
* Client uses passive mode
* Client/Server both support LittleFS and SPIFFS
* Server (fully) supports directories with LittleFS
* Client supports directories with either filesystem 
  since both FS will just auto-create missing Directories
  when accessing files.

## Limitations
* Server only allows one ftp control and one data connection at a time. You need to setup Filezilla (or other clients) to respect that, i.e. only allow **1** connection. (In FileZilla go to File/Site Manager then select your site. In Transfer Settings, check "Limit number of simultaneous connections" and set the maximum to 1.)
* It does not yet support encryption

## Server Usage

### Construct an FTPServer
Select the desired FS via the contructor 
```cpp
#include <FTPServer.h>
#include <LittleFS.h>

FTPServer ftpSrv(LittleFS); // construct with LittleFS
// or
FTPServer ftpSrv(SPIFFS);   // construct with SPIFFS if you need to for backward compatibility
```

### Set username/password
```cpp
ftpSrv.begin("username", "password");
```

### Handle by calling frequently
```cpp
ftpSrv.handleFTP(); // place this in e.g. loop()
```

## Client Usage

### Construct an FTPClient
Select the desired FS via the contructor 
```cpp
#include <FTPClient.h>
#include <LittleFS.h>

FTPClient ftpClient(LittleFS); // construct with LittleFS
// or
FTPClient ftpClient(SPIFFS);   // construct with SPIFFS if you need to for backward compatibility
```

### Provide username, password, server, port...
```cpp
// struct ServerInfo
// {
//     String login;
//     String password;
//     String servername;
//     uint16_t port;
//     bool authTLS = false;
//     bool validateCA = false;
// };

ServerInfo ftpServerInfo ("username", "password", "server_name_or_ip", 21);
ftpClient.begin(ftpServerInfo);
```

### Transfer a file
```cpp
ftpClient.transfer("local_file_path", "remote_file_path", FTPClient::FTP_GET);  // get a file blocking
ftpClient.transfer("local_file_path", "remote_file_path", FTPClient::FTP_PUT_NONBLOCKING);  // put a file non-blocking
```
### Handle non-blocking transfers by calling frequently
```cpp
ftpClient.handleFTP(); // place this in e.g. loop()
```

## Notes
* I forked the Server from https://github.com/nailbuster/esp8266FTPServer which itself was forked from: https://github.com/gallegojm/Arduino-Ftp-Server/tree/master/FtpServer
* Inspiration for the Client was taken from https://github.com/esp8266/Arduino/issues/1183#issuecomment-634556135
