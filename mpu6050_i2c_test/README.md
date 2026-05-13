# MPU6050 I2C Verify

这个分支用于单独验证 MPU6050 的 I2C 通信，不依赖 MPU6050 内核驱动。

默认参数：

```text
I2C device: /dev/i2c-2
I2C addr:   0x68
```

## Build

在远程 Linux 编译环境中：

```sh
make CROSS_COMPILE=arm-v01c02-linux-musleabi-
```

也可以本机 native 编译做语法验证：

```sh
make
```

## Run

板端运行：

```sh
./mpu6050_i2c_test
./mpu6050_i2c_test /dev/i2c-2 0x68
```

输出包含：

```text
raw accel / temp / gyro
val accel(g) / temp(C) / gyro(dps)
```
