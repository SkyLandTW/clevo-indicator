/*
 ============================================================================
 Name        : clevo-indicator.c
 Author      : AqD <iiiaqd@gmail.com>
 Version     :
 Description : Ubuntu fan control indicator for Clevo laptops

 Based on http://www.association-apml.fr/upload/fanctrl.c by Jonas Diemer
 (diemer@gmx.de)

 ============================================================================

 Compile: "gcc clevo-indicator.c -o clevo-indicator"
 Run as root.

 ============================================================================
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/io.h>
#include <unistd.h>

#define EC_SC 0x66
#define EC_DATA 0x62

#define IBF 1
#define OBF 0
#define EC_SC_READ_CMD 0x80

static int init_ec(void);
static void dump_fan_config(void);
static void test_fan_config(int duty_percentage);
static int wait_ec(const uint32_t port, const uint32_t flag, const char value);
static uint8_t read_ec(const uint32_t port);
static void do_ec(const uint32_t cmd, const uint32_t port, const uint8_t value);

int main(int argc, char* argv[])
{
	printf("Simple fan control utility for Clevo laptops\n");
	if (init_ec() != EXIT_SUCCESS)
	{
		printf("ioperm() failed!\n");
		return EXIT_FAILURE;
	}
	if (argc <= 1)
	{
		char* display = getenv("DISPLAY");
		if (display == NULL || strlen(display) == 0)
		{
			dump_fan_config();
		}
		else
		{
			// indicator
		}
	}
	else
	{
		int val = atoi(argv[1]);
		if (val < 40 || val > 100)
		{
			printf("invalid fan duty %d!\n", val);
			return EXIT_FAILURE;
		}
		test_fan_config(val);
	}
	return EXIT_SUCCESS;
}

static int init_ec(void)
{
	if (ioperm(EC_DATA, 1, 1) != 0)
		return EXIT_FAILURE;
	if (ioperm(EC_SC, 1, 1) != 0)
		return EXIT_FAILURE;
	return EXIT_SUCCESS;
}

static void dump_fan_config(void)
{
	printf("Dump FAN\n");
	int raw_duty = read_ec(0xCE);
	int val_duty = (int) ((double) raw_duty / 255.0 * 100.0);
	int raw_rpm = (read_ec(0xD0) << 8) + (read_ec(0xD1));
	int val_rpm = 2156220 / raw_rpm;
	printf("FAN Duty: %d%%\n", val_duty);
	printf("FAN RPMs: %d RPM\n", val_rpm);
	printf("CPU Temp: %d°C\n", read_ec(0x07));
}

static void test_fan_config(int duty_percentage)
{
	double v_d = ((double) duty_percentage) / 100.0 * 255.0;
	int v_i = (int) v_d;
	printf("Test FAN %d%% to %d\n", duty_percentage, v_i);
	do_ec(0x99, 0x01, v_i);
	dump_fan_config();
}

static int wait_ec(const uint32_t port, const uint32_t flag, const char value)
{
	uint8_t data = inb(port);
	int i = 0;
	while ((((data >> flag) & 0x1) != value) && (i++ < 100))
	{
		usleep(1000);
		data = inb(port);
	}
	if (i >= 100)
	{
		printf("wait_ec error on port 0x%x, data=0x%x, flag=0x%x, value=0x%x\n",
				port, data, flag, value);
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}

static uint8_t read_ec(const uint32_t port)
{
	wait_ec(EC_SC, IBF, 0);
	outb(EC_SC_READ_CMD, EC_SC);

	wait_ec(EC_SC, IBF, 0);
	outb(port, EC_DATA);

	//wait_ec(EC_SC, EC_SC_IBF_FREE);
	wait_ec(EC_SC, OBF, 1);
	uint8_t value = inb(EC_DATA);

	return value;
}

static void do_ec(const uint32_t cmd, const uint32_t port, const uint8_t value)
{
	wait_ec(EC_SC, IBF, 0);
	outb(cmd, EC_SC);

	wait_ec(EC_SC, IBF, 0);
	outb(port, EC_DATA);

	wait_ec(EC_SC, IBF, 0);
	outb(value, EC_DATA);

	wait_ec(EC_SC, IBF, 0);

	return;
}
