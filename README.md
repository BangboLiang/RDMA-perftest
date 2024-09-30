# RDMA perf test in C codes

## Author Bangbo Liang lbb@hnu.edu.cn

Now Version 8 is an avaliable version, the code is still being testing and programming. :)  

## Attetion!  
Version 4 only send is avaliable, 
Version 8 read and write is avaliable.  

Version 9 is programming, will support all operations.

Usage:  
 ./MyPerfv8 start a server and wait for connection  
 ./MyPerfv8 `<host>` connect to server at `<host>`  

Options:  
 -p, --port `<port>` listen on/connect to port `<port>` (default 18515)  

 -d, --ib-dev `<dev>` use IB device `<dev>` (default first device found)  

 -i, --ib-port `<port>` use port `<port>` of IB device (default 1)  

 -g, --gid_idx `<git index>` gid index to be used in GRH (default not used)  

 -t, --test_interval `<interval>` perf test interval in us  

 -o, --test_opcode `<opcode>` 2 for send, 4 for read, 0 for write  

 -s, --test_times `<num>` number of test times  
 
 -e, --iter_nums `<num>` number of iterations per test round(less than 1000)  

 -m, --Message_size `<Bytes>` Message's size in Bytes, default 65536.