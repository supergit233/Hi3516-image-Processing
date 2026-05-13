/*
 * MPU6050 用户态 I2C 通信测试程序。
 *
 * 这个程序不需要 MPU6050 内核驱动，直接通过 Linux 的 /dev/i2c-X
 * 用户态接口和芯片通信。
 *
 * 默认总线：/dev/i2c-2
 * 默认地址：0x68
 *
 * 编译：
 *   make
 *
 * 运行：
 *   ./mpu6050_i2c_test
 *   ./mpu6050_i2c_test 0x69
 *   ./mpu6050_i2c_test /dev/i2c-1 0x68
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define MPU6050_I2C_DEV       "/dev/i2c-2"
#define MPU6050_DEFAULT_ADDR  0x68

#define MPU6050_REG_PWR_MGMT1 0x6B  /* 写 0 唤醒芯片。 */
#define MPU6050_REG_WHO_AM_I  0x75  /* MPU6050 正常应读回 0x68。 */
#define MPU6050_REG_ACCEL_X_H 0x3B  /* 14 字节传感器数据块的起始寄存器。 */

#define MPU6050_WHO_AM_I_VAL  0x68
#define MPU6050_SAMPLE_US     200000

/* 默认满量程：加速度 +/-2g，陀螺仪 +/-250 deg/s。 */
#define MPU6050_ACCEL_SCALE   16384.0
#define MPU6050_GYRO_SCALE    131.0

static volatile sig_atomic_t g_running = 1;

typedef struct {
    /* 0x3B 到 0x48 的寄存器顺序：加速度、温度、陀螺仪。 */
    int16_t accel_x;
    int16_t accel_y;
    int16_t accel_z;
    int16_t temp;
    int16_t gyro_x;
    int16_t gyro_y;
    int16_t gyro_z;
} mpu6050_raw_data;

static void handle_signal(int signo)
{
    (void)signo;
    g_running = 0;
}

static int parse_i2c_addr(const char *text, int *addr)
{
    char *end = NULL;
    unsigned long value;

    /* base 设为 0，可以同时支持 "104" 和 "0x68" 这种输入。 */
    errno = 0;
    value = strtoul(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0' || value > 0x7f) {
        return -1;
    }

    *addr = (int)value;
    return 0;
}

static int i2c_write_u8(int fd, uint8_t reg, uint8_t value)
{
    uint8_t data[2] = { reg, value };
    ssize_t written = write(fd, data, sizeof(data));

    if (written != (ssize_t)sizeof(data)) {
        return -1;
    }

    return 0;
}

static int i2c_read_bytes(int fd, int addr, uint8_t reg, uint8_t *buf, size_t len)
{
    struct i2c_msg msgs[2];
    struct i2c_rdwr_ioctl_data data;

    /*
     * 常见 I2C 寄存器读取时序：
     * 1. 先写入要读取的起始寄存器地址
     * 2. 再从这个寄存器开始连续读取 len 个字节
     *
     * 这里使用 I2C_RDWR 一次 ioctl 完成两段传输，中间是 repeated-start，
     * 比 write()+read() 更接近传感器手册里的标准寄存器读取时序。
     */
    msgs[0].addr = addr;
    msgs[0].flags = 0;
    msgs[0].len = 1;
    msgs[0].buf = &reg;

    msgs[1].addr = addr;
    msgs[1].flags = I2C_M_RD;
    msgs[1].len = len;
    msgs[1].buf = buf;

    data.msgs = msgs;
    data.nmsgs = 2;

    if (ioctl(fd, I2C_RDWR, &data) < 0) {
        return -1;
    }

    return 0;
}

static int i2c_read_u8(int fd, int addr, uint8_t reg, uint8_t *value)
{
    return i2c_read_bytes(fd, addr, reg, value, 1);
}

static int mpu6050_read_raw(int fd, int addr, mpu6050_raw_data *raw)
{
    uint8_t buf[14];

    /* 一次性连续读取所有传感器输出，保证各轴数据属于同一次采样。 */
    if (i2c_read_bytes(fd, addr, MPU6050_REG_ACCEL_X_H, buf, sizeof(buf)) != 0) {
        return -1;
    }

    /* MPU6050 每个 16 位数据都是高字节在前、低字节在后。 */
    raw->accel_x = (int16_t)((buf[0] << 8) | buf[1]);
    raw->accel_y = (int16_t)((buf[2] << 8) | buf[3]);
    raw->accel_z = (int16_t)((buf[4] << 8) | buf[5]);
    raw->temp = (int16_t)((buf[6] << 8) | buf[7]);
    raw->gyro_x = (int16_t)((buf[8] << 8) | buf[9]);
    raw->gyro_y = (int16_t)((buf[10] << 8) | buf[11]);
    raw->gyro_z = (int16_t)((buf[12] << 8) | buf[13]);

    return 0;
}

static void print_raw_data(const mpu6050_raw_data *raw)
{
    /* 这些换算公式对应 MPU6050 基础示例里常用的默认量程。 */
    double accel_x_g = raw->accel_x / MPU6050_ACCEL_SCALE;
    double accel_y_g = raw->accel_y / MPU6050_ACCEL_SCALE;
    double accel_z_g = raw->accel_z / MPU6050_ACCEL_SCALE;
    double temp_c = raw->temp / 340.0 + 36.53;
    double gyro_x_dps = raw->gyro_x / MPU6050_GYRO_SCALE;
    double gyro_y_dps = raw->gyro_y / MPU6050_GYRO_SCALE;
    double gyro_z_dps = raw->gyro_z / MPU6050_GYRO_SCALE;

    printf("raw accel=(%6d,%6d,%6d) temp=%6d gyro=(%6d,%6d,%6d)\n",
        raw->accel_x, raw->accel_y, raw->accel_z,
        raw->temp, raw->gyro_x, raw->gyro_y, raw->gyro_z);
    printf("val accel=(%7.3f,%7.3f,%7.3f)g temp=%6.2fC gyro=(%7.3f,%7.3f,%7.3f)dps\n",
        accel_x_g, accel_y_g, accel_z_g,
        temp_c, gyro_x_dps, gyro_y_dps, gyro_z_dps);
}

static void print_usage(const char *prog)
{
    fprintf(stderr, "Usage: %s [i2c_addr]\n", prog);
    fprintf(stderr, "Usage: %s [i2c_dev] [i2c_addr]\n", prog);
    fprintf(stderr, "Example: %s\n", prog);
    fprintf(stderr, "Example: %s 0x69\n", prog);
    fprintf(stderr, "Example: %s /dev/i2c-1 0x68\n", prog);
}

int main(int argc, char *argv[])
{
    int fd;
    int addr = MPU6050_DEFAULT_ADDR;
    const char *i2c_dev = MPU6050_I2C_DEV;
    uint8_t who_am_i = 0;

    if (argc > 3) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    /*
     * 兼容两种用法：
     * 1. 只传地址：./mpu6050_i2c_test 0x69
     * 2. 同时传总线和地址：./mpu6050_i2c_test /dev/i2c-2 0x68
     */
    if (argc == 2 && parse_i2c_addr(argv[1], &addr) != 0) {
        fprintf(stderr, "invalid i2c address: %s\n", argv[1]);
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (argc == 3) {
        i2c_dev = argv[1];
        if (parse_i2c_addr(argv[2], &addr) != 0) {
            fprintf(stderr, "invalid i2c address: %s\n", argv[2]);
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    printf("MPU6050 I2C test\n");
    printf("device: %s, addr: 0x%02x\n", i2c_dev, addr);

    fd = open(i2c_dev, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "open %s failed: %s\n", i2c_dev, strerror(errno));
        return EXIT_FAILURE;
    }

    /*
     * 设置后续读写使用的 MPU6050 从机地址。
     * AD0 接低电平是 0x68，AD0 接高电平是 0x69。
     */
    if (ioctl(fd, I2C_SLAVE, addr) < 0) {
        fprintf(stderr, "ioctl I2C_SLAVE 0x%02x failed: %s\n", addr, strerror(errno));
        close(fd);
        return EXIT_FAILURE;
    }

    /* 修改配置前，先读 WHO_AM_I 确认设备确实有响应。 */
    if (i2c_read_u8(fd, addr, MPU6050_REG_WHO_AM_I, &who_am_i) != 0) {
        fprintf(stderr, "read WHO_AM_I failed: %s\n", strerror(errno));
        close(fd);
        return EXIT_FAILURE;
    }

    printf("WHO_AM_I: 0x%02x\n", who_am_i);
    if (who_am_i != MPU6050_WHO_AM_I_VAL) {
        fprintf(stderr, "warning: expected WHO_AM_I 0x%02x, got 0x%02x\n",
            MPU6050_WHO_AM_I_VAL, who_am_i);
    }

    /* MPU6050 上电后默认睡眠，清零 PWR_MGMT_1 可以唤醒。 */
    if (i2c_write_u8(fd, MPU6050_REG_PWR_MGMT1, 0x00) != 0) {
        fprintf(stderr, "wake MPU6050 failed: %s\n", strerror(errno));
        close(fd);
        return EXIT_FAILURE;
    }

    /* 唤醒后给陀螺仪和加速度计一点稳定时间。 */
    usleep(100000);
    printf("MPU6050 wake ok, press Ctrl+C to stop.\n");

    while (g_running) {
        mpu6050_raw_data raw;

        if (mpu6050_read_raw(fd, addr, &raw) != 0) {
            fprintf(stderr, "read sensor data failed: %s\n", strerror(errno));
            close(fd);
            return EXIT_FAILURE;
        }

        print_raw_data(&raw);
        usleep(MPU6050_SAMPLE_US);
    }

    close(fd);
    printf("exit\n");
    return EXIT_SUCCESS;
}
