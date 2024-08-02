# RDMA perf test in C codes

## author Bangbo Liang lbb@hnu.edu.cn

Now v3 is an avaliable version, the code is still being tested and programming. :)

Usage:
 ./MyPerfv3 start a server and wait for connection
 ./MyPerfv3 <host> connect to server at <host>

Options:
 -p, --port <port> listen on/connect to port <port> (default 18515)
 -d, --ib-dev <dev> use IB device <dev> (default first device found)
 -i, --ib-port <port> use port <port> of IB device (default 1)
 -g, --gid_idx <git index> gid index to be used in GRH (default not used)
 -t, --test_interval <interval> perf test interval in us
 -o, --test_opcode <opcode> 2 for send, 4 for read, 0 for write
 -s, --test_times <num> number of test times
 -e, --iter_nums <num> number of iterations per test round