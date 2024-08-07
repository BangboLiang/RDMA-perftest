import matplotlib.pyplot as plt



filename = '../Results/NP4-10G-1s-1us.txt'

# import files
def parse_data(filename):
    speeds = []
    times = []
    last_time = 0
    with open(filename, 'r') as file:
        for line in file:
            parts = line.split(',')
            speed = float(parts[0].split(':')[1].strip().split()[0])
            time =  float(parts[1].split(':')[1].strip().split()[0])
            time = time + last_time
            last_time = time
            time = time / 1000.0
            speeds.append(speed)
            times.append(time)
    return speeds, times

# plot
def plot_data(speeds, times):
    plt.figure(figsize=(10, 5))
    plt.plot(times, speeds, marker='o', linestyle='-')
    plt.title('Throughput Over Time')
    plt.xlabel('Time (ms)')
    plt.ylabel('Throughput (Gbit/s)')
    plt.grid(True)
    plt.savefig('testfile.png')
    plt.close()

# main function
def main():
    speeds, times = parse_data(filename)
    plot_data(speeds, times)

if __name__ == '__main__':
    main()