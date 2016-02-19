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
 Auto fan control algorithm:

 The algorithm is to replace the builtin auto fan-control algorithm in Clevo
 laptops which is apparently broken in recent models such as W350SSQ, where the
 fan doesn't get kicked until both of GPU and CPU are really hot (and GPU
 cannot be hot anymore thanks to nVIDIA's Maxwell chips). It's far more
 aggressive than the builtin algorithm in order to keep the temperatures below
 60°C all the time, for maximized performance with Intel turbo boost enabled.

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

/* EC registers can be read by EC_SC_READ_CMD or /sys/kernel/debug/ec/ec0/io:
 *
 * 1. modprobe ec_sys
 * 2. od -Ax -t x1 /sys/kernel/debug/ec/ec0/io
 */

#define EC_REG_SIZE 0x100
#define EC_REG_CPU_TEMP 0x07
#define EC_REG_GPU_TEMP 0xCD
#define EC_REG_FAN_DUTY 0xCE
#define EC_REG_FAN_RPMS_HI 0xD0
#define EC_REG_FAN_RPMS_LO 0xD1

#define MAX_FAN_RPM 4400.0

typedef enum
{
	NA = 0, AUTO = 1, MANUAL = 2
} MenuItemType;

static void main_init_share(void);
static int main_ec_worker(void);
static void main_ui_worker(int argc, char** argv);
static void main_on_sigchld(int signum);
static void main_on_sigterm(int signum);
static int main_dump_fan(void);
static int main_test_fan(int duty_percentage);
static gboolean ui_update(gpointer user_data);
static void ui_command_set_fan(long fan_duty);
static void ui_command_quit(gchar* command);
static void ui_toggle_menuitems(int fan_duty);
static void ec_on_sigterm(int signum);
static int ec_init(void);
static int ec_auto_duty_adjust(void);
static int ec_query_cpu_temp(void);
static int ec_query_gpu_temp(void);
static int ec_query_fan_duty(void);
static int ec_query_fan_rpms(void);
static int ec_write_fan_duty(int duty_percentage);
static int ec_io_wait(const uint32_t port, const uint32_t flag,
		const char value);
static uint8_t ec_io_read(const uint32_t port);
static int ec_io_do(const uint32_t cmd, const uint32_t port,
		const uint8_t value);
static int calculate_fan_duty(int raw_duty);
static int calculate_fan_rpms(int raw_rpm_high, int raw_rpm_low);
static void get_time_string(char* buffer, size_t max, const char* format);
static void signal_term(__sighandler_t handler);

static AppIndicator* indicator = NULL;

struct
{
	char label[256];
	GCallback callback;
	long option;
	MenuItemType type;
	GtkWidget* widget;

} menuitems[] =
{
{ "Set FAN to AUTO", G_CALLBACK(ui_command_set_fan), 0, AUTO, NULL },
{ "", NULL, 0L, NA, NULL },
{ "Set FAN to  60%", G_CALLBACK(ui_command_set_fan), 60, MANUAL, NULL },
{ "Set FAN to  70%", G_CALLBACK(ui_command_set_fan), 70, MANUAL, NULL },
{ "Set FAN to  80%", G_CALLBACK(ui_command_set_fan), 80, MANUAL, NULL },
{ "Set FAN to  90%", G_CALLBACK(ui_command_set_fan), 90, MANUAL, NULL },
{ "Set FAN to 100%", G_CALLBACK(ui_command_set_fan), 100, MANUAL, NULL },
{ "", NULL, 0L, NA, NULL },
{ "Quit", G_CALLBACK(ui_command_quit), 0L, NA, NULL } };

static int menuitem_count = (sizeof(menuitems) / sizeof(menuitems[0]));

struct
{
	volatile int exit;
	volatile int cpu_temp;
	volatile int gpu_temp;
	volatile int fan_duty;
	volatile int fan_rpms;
	volatile int auto_duty;
	volatile int auto_duty_val;
	volatile int manual_next_fan_duty;
	volatile int manual_prev_fan_duty;
}*share_info = NULL;

int main(int argc, char* argv[])
{
	printf("Simple fan control utility for Clevo laptops\n");
	if (ec_init() != EXIT_SUCCESS)
	{
		printf("unable to control EC: %s\n", strerror(errno));
		return EXIT_FAILURE;
	}
	if (argc <= 1)
	{
		char* display = getenv("DISPLAY");
		if (display == NULL || strlen(display) == 0)
		{
			return main_dump_fan();
		}
		else
		{
			main_init_share();
			signal(SIGCHLD, &main_on_sigchld);
			signal_term(&main_on_sigterm);
			pid_t worker_pid = fork();
			if (worker_pid == 0)
			{
				signal(SIGCHLD, SIG_DFL);
				signal_term(&ec_on_sigterm);
				return main_ec_worker();
			}
			else if (worker_pid > 0)
			{
				main_ui_worker(argc, argv);
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
		if (argv[1][0] == '-')
		{
			printf(
					"\n\
Usage: clevo-indicator [fan-duty-percentage]\n\
\n\
Dump/Control fan duty on Clevo laptops. Display indicator by default.\n\
\n\
Arguments:\n\
  [fan-duty-percentage]\t\tTarget fan duty in percentage, from 40 to 100\n\
  -?\t\t\t\tDisplay this help and exit\n\
\n\
Without arguments this program should attempt to display an indicator in\n\
the Ubuntu tray area for fan information display and control. The indicator\n\
requires this program to have setuid=root flag but run from the desktop user\n\
, because a root user is not allowed to display a desktop indicator while a\n\
non-root user is not allowed to control Clevo EC (Embedded Controller that's\n\
responsible of the fan). Fix permissions of this executable if it fails to\n\
run:\n\
    sudo chown root clevo-indicator\n\
    sudo chmod u+s  clevo-indicator\n\
\n\
Note any fan duty change should take 1-2 seconds to come into effect - you\n\
can verify by the fan speed displayed on indicator icon and also louder fan\n\
noise.\n\
\n\
In the indicator mode, this program would always attempt to load kernel\n\
module 'ec_sys', in order to query EC information from\n\
'/sys/kernel/debug/ec/ec0/io' instead of polling EC ports for readings,\n\
which may be more risky if interrupted or concurrently operated during the\n\
process.\n\
\n\
DO NOT MANIPULATE OR QUERY EC I/O PORTS WHILE THIS PROGRAM IS RUNNING.\n\
\n");
			return main_dump_fan();
		}
		else
		{
			int val = atoi(argv[1]);
			if (val < 40 || val > 100)
			{
				printf("invalid fan duty %d!\n", val);
				return EXIT_FAILURE;
			}
			return main_test_fan(val);
		}
	}
	return EXIT_SUCCESS;
}

static void main_init_share(void)
{
	void* shm = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_ANON | MAP_SHARED,
			-1, 0);
	share_info = shm;
	share_info->exit = 0;
	share_info->cpu_temp = 0;
	share_info->gpu_temp = 0;
	share_info->fan_duty = 0;
	share_info->fan_rpms = 0;
	share_info->auto_duty = 1;
	share_info->auto_duty_val = 0;
	share_info->manual_next_fan_duty = 0;
	share_info->manual_prev_fan_duty = 0;
}

static int main_ec_worker(void)
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
			ec_write_fan_duty(new_fan_duty);
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
			share_info->gpu_temp = buf[EC_REG_GPU_TEMP];
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
		// auto
		if (share_info->auto_duty == 1)
		{
			int next_duty = ec_auto_duty_adjust();
			if (next_duty != 0 && next_duty != share_info->auto_duty_val)
			{
				char s_time[256];
				get_time_string(s_time, 256, "%m/%d %H:%M:%S");
				printf("%s CPU=%d°C, GPU=%d°C, auto fan duty to %d%%\n", s_time,
						share_info->cpu_temp, share_info->gpu_temp, next_duty);
				ec_write_fan_duty(next_duty);
				share_info->auto_duty_val = next_duty;
			}
		}
		//
		usleep(500);
	}
	printf("worker quit\n");
	return EXIT_SUCCESS;
}

static void main_ui_worker(int argc, char** argv)
{
	printf("Indicator...\n");
	int desktop_uid = getuid();
	setuid(desktop_uid);
	//
	gtk_init(&argc, &argv);
	//
	GtkWidget* indicator_menu = gtk_menu_new();
	for (int i = 0; i < menuitem_count; i++)
	{
		GtkWidget* item;
		if (strlen(menuitems[i].label) == 0)
		{
			item = gtk_separator_menu_item_new();
		}
		else
		{
			item = gtk_menu_item_new_with_label(menuitems[i].label);
			g_signal_connect_swapped(item, "activate",
					G_CALLBACK(menuitems[i].callback),
					(void*) menuitems[i].option);
		}
		gtk_menu_shell_append(GTK_MENU_SHELL(indicator_menu), item);
		menuitems[i].widget = item;
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
	g_timeout_add(500, &ui_update, NULL);
	ui_toggle_menuitems(share_info->fan_duty);
	gtk_main();
	printf("main on UI quit\n");
}

static void main_on_sigchld(int signum)
{
	printf("main on worker quit signal\n");
	exit (EXIT_SUCCESS);
}

static void main_on_sigterm(int signum)
{
	printf("main on signal: %s\n", strsignal(signum));
	if (share_info != NULL)
		share_info->exit = 1;
	exit (EXIT_SUCCESS);
}

static int main_dump_fan(void)
{
	printf("Dump fan information\n");
	printf("  FAN Duty: %d%%\n", ec_query_fan_duty());
	printf("  FAN RPMs: %d RPM\n", ec_query_fan_rpms());
	printf("  CPU Temp: %d°C\n", ec_query_cpu_temp());
	printf("  GPU Temp: %d°C\n", ec_query_gpu_temp());
	return EXIT_SUCCESS;
}

static int main_test_fan(int duty_percentage)
{
	printf("Change fan duty to %d%%\n", duty_percentage);
	ec_write_fan_duty(duty_percentage);
	printf("\n");
	main_dump_fan();
	return EXIT_SUCCESS;
}

static gboolean ui_update(gpointer user_data)
{
	char label[256];
	sprintf(label, "%d℃ %d℃", share_info->cpu_temp, share_info->gpu_temp);
	app_indicator_set_label(indicator, label, "XXXXXX");
	char icon_name[256];
	double load = ((double) share_info->fan_rpms) / MAX_FAN_RPM * 100.0;
	double load_r = round(load / 5.0) * 5.0;
	sprintf(icon_name, "brasero-disc-%02d", (int) load_r);
	app_indicator_set_icon(indicator, icon_name);
	return G_SOURCE_CONTINUE;
}

static void ui_command_set_fan(long fan_duty)
{
	int fan_duty_val = (int) fan_duty;
	if (fan_duty_val == 0)
	{
		printf("clicked on fan duty auto\n");
		share_info->auto_duty = 1;
		share_info->manual_next_fan_duty = 0;
	}
	else
	{
		printf("clicked on fan duty: %d\n", fan_duty_val);
		share_info->auto_duty = 0;
		share_info->manual_next_fan_duty = fan_duty_val;
	}
	ui_toggle_menuitems(fan_duty_val);
}

static void ui_command_quit(gchar* command)
{
	printf("clicked on quit\n");
	gtk_main_quit();
}

static void ui_toggle_menuitems(int fan_duty)
{
	for (int i = 0; i < menuitem_count; i++)
	{
		if (menuitems[i].widget == NULL)
			continue;
		if (fan_duty == 0)
			gtk_widget_set_sensitive(menuitems[i].widget,
					menuitems[i].type != AUTO);
		else
			gtk_widget_set_sensitive(menuitems[i].widget,
					menuitems[i].type != MANUAL
							|| (int) menuitems[i].option != fan_duty);
	}
}

static int ec_init(void)
{
	if (ioperm(EC_DATA, 1, 1) != 0)
		return EXIT_FAILURE;
	if (ioperm(EC_SC, 1, 1) != 0)
		return EXIT_FAILURE;
	return EXIT_SUCCESS;
}

static void ec_on_sigterm(int signum)
{
	printf("ec on signal: %s\n", strsignal(signum));
	if (share_info != NULL)
		share_info->exit = 1;
}

static int ec_auto_duty_adjust(void)
{
	int temp = MAX(share_info->cpu_temp, share_info->gpu_temp);
	int duty = share_info->fan_duty;
	//
	if (temp >= 80 && duty < 100)
		return 100;
	if (temp >= 70 && (duty < 90 || (temp <= 75 && duty > 90)))
		return 90;
	if (temp >= 60 && (duty < 80 || (temp <= 65 && duty > 80)))
		return 80;
	if (temp >= 50 && (duty < 70 || (temp <= 55 && duty > 70)))
		return 70;
	if (temp >= 40 && (duty < 60 || (temp <= 45 && duty > 60)))
		return 60;
	if (temp >= 30 && (duty < 50 || (temp <= 35 && duty > 50)))
		return 50;
	//
	return 0;
}

static int ec_query_cpu_temp(void)
{
	return ec_io_read(EC_REG_CPU_TEMP);
}

static int ec_query_gpu_temp(void)
{
	return ec_io_read(EC_REG_GPU_TEMP);
}

static int ec_query_fan_duty(void)
{
	int raw_duty = ec_io_read(EC_REG_FAN_DUTY);
	return calculate_fan_duty(raw_duty);
}

static int ec_query_fan_rpms(void)
{
	int raw_rpm_hi = ec_io_read(EC_REG_FAN_RPMS_HI);
	int raw_rpm_lo = ec_io_read(EC_REG_FAN_RPMS_LO);
	return calculate_fan_rpms(raw_rpm_hi, raw_rpm_lo);
}

static int ec_write_fan_duty(int duty_percentage)
{
	if (duty_percentage < 60 || duty_percentage > 100)
	{
		printf("Wrong fan duty to write: %d\n", duty_percentage);
		return EXIT_FAILURE;
	}
	double v_d = ((double) duty_percentage) / 100.0 * 255.0;
	int v_i = (int) v_d;
	return ec_io_do(0x99, 0x01, v_i);
}

static int ec_io_wait(const uint32_t port, const uint32_t flag,
		const char value)
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

static uint8_t ec_io_read(const uint32_t port)
{
	ec_io_wait(EC_SC, IBF, 0);
	outb(EC_SC_READ_CMD, EC_SC);

	ec_io_wait(EC_SC, IBF, 0);
	outb(port, EC_DATA);

	//wait_ec(EC_SC, EC_SC_IBF_FREE);
	ec_io_wait(EC_SC, OBF, 1);
	uint8_t value = inb(EC_DATA);

	return value;
}

static int ec_io_do(const uint32_t cmd, const uint32_t port,
		const uint8_t value)
{
	ec_io_wait(EC_SC, IBF, 0);
	outb(cmd, EC_SC);

	ec_io_wait(EC_SC, IBF, 0);
	outb(port, EC_DATA);

	ec_io_wait(EC_SC, IBF, 0);
	outb(value, EC_DATA);

	return ec_io_wait(EC_SC, IBF, 0);
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

static void get_time_string(char* buffer, size_t max, const char* format)
{
	time_t timer;
	struct tm tm_info;
	time(&timer);
	localtime_r(&timer, &tm_info);
	strftime(buffer, max, format, &tm_info);
}

static void signal_term(__sighandler_t handler)
{
	signal(SIGHUP, handler);
	signal(SIGINT, handler);
	signal(SIGQUIT, handler);
	signal(SIGPIPE, handler);
	signal(SIGALRM, handler);
	signal(SIGTERM, handler);
	signal(SIGUSR1, handler);
	signal(SIGUSR2, handler);
}
