# Simple Dropbox Clone — Phase 1

## Build
$ make

## Run server
$ ./server 9000
Server listening on port 9000

## Run client (in another terminal)
$ ./client 127.0.0.1 9000 atique
> LIST
> UPLOAD local.txt local.txt
> LIST
> DOWNLOAD local.txt copy.txt
> DELETE local.txt
> BYE
 
