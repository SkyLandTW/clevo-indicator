/*
 ============================================================================
 Name        : clevo-indicator.c
 Author      : AqD <iiiaqd@gmail.com>
 Version     :
 Description : Ubuntu fan control indicator for Clevo laptops

 Based on http://www.association-apml.fr/upload/fanctrl.c by Jonas Diemer
 (diemer@gmx.de)

 ============================================================================

 TEST:
 gcc clevo-indicator.c -o clevo-indicator `pkg-config --cflags --libs appindicator3-0.1` -lm
 sudo chown root clevo-indicator
 sudo chmod u+s clevo-indicator

 Run as effective uid = root, but uid = desktop user (in order to use indicator).

 ============================================================================
 */

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/io.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <libappindicator/app-indicator.h>

#define EC_SC 0x66
#define EC_DATA 0x62

#define IBF 1
#define OBF 0
#define EC_SC_READ_CMD 0x80

#define EC_REG_SIZE 0x100
#define EC_REG_CPU_TEMP 0x07
#define EC_REG_FAN_DUTY 0xCE
#define EC_REG_FAN_RPMS_HI 0xD0
#define EC_REG_FAN_RPMS_LO 0xD1

#define MAX_FAN_RPM 4400.0

static int init_ec(void);
static int check_value(const char* arg);
static int main_fan_worker(void);
static void main_indicator(int argc, char** argv);
static void main_worker_exit(int signum);
static gboolean main_update_ui(gpointer user_data);
static int main_dump_fan_config(void);
static int main_test_fan_config(int duty_percentage);
static void menuitem_set_fan(long fan_duty);
static void menuitem_quit(gchar* command);
static int query_cpu_temp(void);
static int query_fan_duty(void);
static int query_fan_rpms(void);
static int calculate_fan_duty(int raw_duty);
static int calculate_fan_rpms(int raw_rpm_high, int raw_rpm_low);
static int write_fan_duty(int duty_percentage);
static int wait_ec(const uint32_t port, const uint32_t flag, const char value);
static uint8_t read_ec(const uint32_t port);
static int do_ec(const uint32_t cmd, const uint32_t port, const uint8_t value);

static AppIndicator* indicator;

struct
{
	char label[256];
	GCallback callback;
	gpointer cmd;

} menu_item_initializers[6] =
{
{ "Set FAN to  60%", G_CALLBACK(menuitem_set_fan), (gpointer) 60 },
{ "Set FAN to  70%", G_CALLBACK(menuitem_set_fan), (gpointer) 70 },
{ "Set FAN to  80%", G_CALLBACK(menuitem_set_fan), (gpointer) 80 },
{ "Set FAN to  90%", G_CALLBACK(menuitem_set_fan), (gpointer) 90 },
{ "Set FAN to 100%", G_CALLBACK(menuitem_set_fan), (gpointer) 100 },
{ "Quit", G_CALLBACK(menuitem_quit), NULL } };

static int menu_item_count = (sizeof(menu_item_initializers)
		/ sizeof(menu_item_initializers[0]));

struct
{
	volatile int exit;
	volatile int cpu_temp;
	volatile int fan_duty;
	volatile int fan_rpms;
	volatile int manual_next_fan_duty;
	volatile int manual_prev_fan_duty;
}*share_info;

int main(int argc, char* argv[])
{
	printf("Simple fan control utility for Clevo laptops\n");
	if (init_ec() != EXIT_SUCCESS)
	{
		printf("unable to control EC: %s\n", strerror(errno));
		return EXIT_FAILURE;
	}
	if (argc <= 1 || check_value(argv[1]) != 0)
	{
		char* display = getenv("DISPLAY");
		if (display == NULL || strlen(display) == 0)
		{
			return main_dump_fan_config();
		}
		else
		{
			void* shm = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
					MAP_ANON | MAP_SHARED, -1, 0);
			share_info = shm;
			share_info->exit = 0;
			share_info->cpu_temp = 0;
			share_info->fan_duty = 0;
			share_info->fan_rpms = 0;
			share_info->manual_next_fan_duty = 0;
			share_info->manual_prev_fan_duty = 0;
			signal(SIGCHLD, &main_worker_exit);
			pid_t worker_pid = fork();
			if (worker_pid == 0)
			{
				signal(SIGCHLD, SIG_DFL);
				return main_fan_worker();
			}
			else if (worker_pid > 0)
			{
				main_indicator(argc, argv);
				share_info->exit = 1;
				waitpid(worker_pid, NULL, 0);
			}
			else
			{
				printf("unable to create worker: %s\n", strerror(errno));
				return EXIT_FAILURE;
			}
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
		return main_test_fan_config(val);
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

static int check_value(const char* arg)
{
	if (arg == NULL)
		return EXIT_FAILURE;
	if (*arg == '\0')
		return EXIT_FAILURE;
	char c = *arg;
	if (c >= '0' && c <= '9')
		return EXIT_SUCCESS;
	else
		return EXIT_FAILURE;
}

static int main_fan_worker(void)
{
	setuid(0);
	system("modprobe ec_sys");
	while (share_info->exit == 0)
	{
		// write
		int new_fan_duty = share_info->manual_next_fan_duty;
		if (new_fan_duty != 0
				&& new_fan_duty != share_info->manual_prev_fan_duty)
		{
			write_fan_duty(new_fan_duty);
			share_info->manual_prev_fan_duty = new_fan_duty;
		}
		// read
		int io_fd = open("/sys/kernel/debug/ec/ec0/io", O_RDONLY, 0);
		if (io_fd < 0)
		{
			printf("unable to read EC from sysfs: %s\n", strerror(errno));
			exit (EXIT_FAILURE);
		}
		unsigned char buf[EC_REG_SIZE];
		ssize_t len = read(io_fd, buf, EC_REG_SIZE);
		switch (len)
		{
		case -1:
			printf("unable to read EC from sysfs: %s\n", strerror(errno));
			break;
		case 0x100:
			share_info->cpu_temp = buf[EC_REG_CPU_TEMP];
			share_info->fan_duty = calculate_fan_duty(buf[EC_REG_FAN_DUTY]);
			share_info->fan_rpms = calculate_fan_rpms(buf[EC_REG_FAN_RPMS_HI],
					buf[EC_REG_FAN_RPMS_LO]);
			/*
			 printf("temp=%d, duty=%d, rpms=%d\n", share_info->cpu_temp,
			 share_info->fan_duty, share_info->fan_rpms);
			 */
			break;
		default:
			printf("wrong EC size from sysfs: %ld\n", len);
		}
		close(io_fd);
		//
		usleep(500);
	}
	printf("worker quit\n");
	return EXIT_SUCCESS;
}

static void main_indicator(int argc, char** argv)
{
	printf("Indicator...\n");
	int desktop_uid = getuid();
	setuid(desktop_uid);
	//
	gtk_init(&argc, &argv);
	//
	GtkWidget* indicator_menu = gtk_menu_new();
	for (int i = 0; i < menu_item_count; i++)
	{
		GtkWidget* item = gtk_menu_item_new_with_label(
				menu_item_initializers[i].label);
		g_signal_connect_swapped(item, "activate",
				G_CALLBACK(menu_item_initializers[i].callback),
				menu_item_initializers[i].cmd);
		gtk_menu_shell_append(GTK_MENU_SHELL(indicator_menu), item);
	}
	gtk_widget_show_all(indicator_menu);
	//
	indicator = app_indicator_new("clevo-indicator", "brasero",
			APP_INDICATOR_CATEGORY_HARDWARE);
	g_assert(IS_APP_INDICATOR(indicator));
	app_indicator_set_label(indicator, "Init..", "XX");
	app_indicator_set_status(indicator, APP_INDICATOR_STATUS_ATTENTION);
	app_indicator_set_ordering_index(indicator, -2);
	app_indicator_set_title(indicator, "Clevo");
	app_indicator_set_menu(indicator, GTK_MENU(indicator_menu));
	g_timeout_add(500, &main_update_ui, NULL);
	gtk_main();
	printf("quit UI\n");
}

static void main_worker_exit(int signum)
{
	printf("worker exit\n");
	exit (EXIT_SUCCESS);
}

static gboolean main_update_ui(gpointer user_data)
{
	char label[256];
	sprintf(label, "%d ℃", share_info->cpu_temp);
	app_indicator_set_label(indicator, label, "XXX");
	char icon_name[256];
	double load = ((double) share_info->fan_rpms) / MAX_FAN_RPM * 100.0;
	double load_r = round(load / 5.0) * 5.0;
	sprintf(icon_name, "brasero-disc-%02d", (int) load_r);
	app_indicator_set_icon(indicator, icon_name);
	return G_SOURCE_CONTINUE;
}

static int main_dump_fan_config(void)
{
	printf("Dump FAN\n");
	printf("FAN Duty: %d%%\n", query_fan_duty());
	printf("FAN RPMs: %d RPM\n", query_fan_rpms());
	printf("CPU Temp: %d°C\n", query_cpu_temp());
	return EXIT_SUCCESS;
}

static int main_test_fan_config(int duty_percentage)
{
	printf("Test FAN %d%%\n", duty_percentage);
	write_fan_duty(duty_percentage);
	main_dump_fan_config();
	return EXIT_SUCCESS;
}

static void menuitem_set_fan(long fan_duty)
{
	int fan_duty_val = (int) fan_duty;
	printf("click on fan duty: %d\n", fan_duty_val);
	share_info->manual_next_fan_duty = fan_duty_val;
}

static void menuitem_quit(gchar* command)
{
	printf("click on quit\n");
	gtk_main_quit();
}

static int query_cpu_temp(void)
{
	return read_ec(EC_REG_CPU_TEMP);
}

static int query_fan_duty(void)
{
	int raw_duty = read_ec(EC_REG_FAN_DUTY);
	return calculate_fan_duty(raw_duty);
}

static int query_fan_rpms(void)
{
	int raw_rpm_hi = read_ec(EC_REG_FAN_RPMS_HI);
	int raw_rpm_lo = read_ec(EC_REG_FAN_RPMS_LO);
	return calculate_fan_rpms(raw_rpm_hi, raw_rpm_lo);
}

static int calculate_fan_duty(int raw_duty)
{
	return (int) ((double) raw_duty / 255.0 * 100.0);
}

static int calculate_fan_rpms(int raw_rpm_high, int raw_rpm_low)
{
	int raw_rpm = (raw_rpm_high << 8) + raw_rpm_low;
	return raw_rpm > 0 ? (2156220 / raw_rpm) : 0;
}

static int write_fan_duty(int duty_percentage)
{
	if (duty_percentage < 60 || duty_percentage > 100)
	{
		printf("Wrong fan duty to write: %d\n", duty_percentage);
		return EXIT_FAILURE;
	}
	double v_d = ((double) duty_percentage) / 100.0 * 255.0;
	int v_i = (int) v_d;
	return do_ec(0x99, 0x01, v_i);
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

static int do_ec(const uint32_t cmd, const uint32_t port, const uint8_t value)
{
	wait_ec(EC_SC, IBF, 0);
	outb(cmd, EC_SC);

	wait_ec(EC_SC, IBF, 0);
	outb(port, EC_DATA);

	wait_ec(EC_SC, IBF, 0);
	outb(value, EC_DATA);

	return wait_ec(EC_SC, IBF, 0);
}
