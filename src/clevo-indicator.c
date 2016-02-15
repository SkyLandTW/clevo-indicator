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
 gcc clevo-indicator.c -o clevo-indicator `pkg-config --cflags --libs appindicator3-0.1` -pthread
 sudo chown root clevo-indicator
 sudo chmod u+s clevo-indicator

 Run as effective uid = root, but uid = desktop user (in order to use indicator).

 ============================================================================
 */

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/fsuid.h>
#include <sys/io.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <libappindicator/app-indicator.h>

#define EC_SC 0x66
#define EC_DATA 0x62

#define IBF 1
#define OBF 0
#define EC_SC_READ_CMD 0x80

static int init_ec(void);
static int check_value(const char* arg);
static void* main_fan_worker(void* arg);
static void main_indicator(int argc, char** argv);
static gboolean main_update_ui(gpointer user_data);
static int main_dump_fan_config(void);
static int main_test_fan_config(int duty_percentage);
static int read_cpu_temp(void);
static int read_fan_duty(void);
static int read_fan_rpms(void);
static int write_fan_duty(int duty_percentage);
static int wait_ec(const uint32_t port, const uint32_t flag, const char value);
static uint8_t read_ec(const uint32_t port);
static int do_ec(const uint32_t cmd, const uint32_t port, const uint8_t value);

static AppIndicator* indicator;
static int signal_exit = 0;
static int shared_cpu_temp = 0;
static int shared_fan_duty = 0;
static int shared_fan_rpms = 0;

int main(int argc, char* argv[])
{
	printf("Simple fan control utility for Clevo laptops\n");
	if (init_ec() != EXIT_SUCCESS)
	{
		printf("ioperm() failed!\n");
		return EXIT_FAILURE;
	}
	printf("uid=%d, euid=%d\n", getuid(), geteuid());
	int uid = getuid();
	// setuid(uid);
	syscall(SYS_setuid, uid);
	printf("env: %s\n", getenv("HOME"));
	if (argc <= 1 || check_value(argv[1]) != 0)
	{
		char* display = getenv("DISPLAY");
		if (display == NULL || strlen(display) == 0)
		{
			return main_dump_fan_config();
		}
		else
		{
			pthread_t tid;
			pthread_create(&tid, NULL, &main_fan_worker, NULL);
			main_indicator(argc, argv);
			__atomic_store_n(&signal_exit, 1, __ATOMIC_SEQ_CST);
			pthread_join(tid, NULL);
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

static void* main_fan_worker(void* arg)
{
	while (__atomic_load_n(&signal_exit, __ATOMIC_SEQ_CST) == 0)
	{
		__atomic_store_n(&shared_cpu_temp, read_cpu_temp(), __ATOMIC_SEQ_CST);
		__atomic_store_n(&shared_fan_duty, read_fan_duty(), __ATOMIC_SEQ_CST);
		__atomic_store_n(&shared_fan_rpms, read_fan_rpms(), __ATOMIC_SEQ_CST);
		sleep(1);
	}
	printf("worker quit\n");
	return NULL;
}

static void menuitem_response(gchar* wtf)
{
	printf("click on quit\n");
	gtk_main_quit();
}

static void main_indicator(int argc, char** argv)
{
	printf("Indicator...\n");
	gtk_init(&argc, &argv);
	//
	GtkWidget* quit_item = gtk_menu_item_new_with_label("Quit");
	g_signal_connect_swapped(quit_item, "activate",
			G_CALLBACK(menuitem_response), (gpointer) "file.open");
	GtkWidget* indicator_menu = gtk_menu_new();
	gtk_menu_shell_append(GTK_MENU_SHELL(indicator_menu), quit_item);
	gtk_widget_show_all(indicator_menu);
	//
	indicator = app_indicator_new("clevo-indicator", "go-jump",
			APP_INDICATOR_CATEGORY_HARDWARE);
	g_assert(IS_APP_INDICATOR(indicator));
	app_indicator_set_label(indicator, "Hello", "XX");
	app_indicator_set_attention_icon(indicator, "indicator-messages-new");
	app_indicator_set_status(indicator, APP_INDICATOR_STATUS_ATTENTION);
	app_indicator_set_ordering_index(indicator, -2);
	app_indicator_set_icon(indicator, "go-jump");
	app_indicator_set_title(indicator, "Clevo");
	app_indicator_set_menu(indicator, GTK_MENU(indicator_menu));
	g_timeout_add(500, &main_update_ui, NULL);
	gtk_main();
	printf("quit\n");
}

static gboolean main_update_ui(gpointer user_data)
{
	int cpu_temp = __atomic_load_n(&shared_cpu_temp, __ATOMIC_SEQ_CST);
	int fan_duty = __atomic_load_n(&shared_fan_duty, __ATOMIC_SEQ_CST);
	int fan_rpms = __atomic_load_n(&shared_fan_rpms, __ATOMIC_SEQ_CST);
	char label[256];
	sprintf(label, "%d ℃", cpu_temp);
	app_indicator_set_label(indicator, label, "XXX");
	return G_SOURCE_CONTINUE;
}

static int main_dump_fan_config(void)
{
	printf("Dump FAN\n");
	printf("FAN Duty: %d%%\n", read_fan_duty());
	printf("FAN RPMs: %d RPM\n", read_fan_rpms());
	printf("CPU Temp: %d°C\n", read_cpu_temp());
	return EXIT_SUCCESS;
}

static int main_test_fan_config(int duty_percentage)
{
	printf("Test FAN %d%%\n", duty_percentage);
	write_fan_duty(duty_percentage);
	main_dump_fan_config();
	return EXIT_SUCCESS;
}

static int read_cpu_temp(void)
{
	return read_ec(0x07);
}

static int read_fan_duty(void)
{
	int raw_duty = read_ec(0xCE);
	int val_duty = (int) ((double) raw_duty / 255.0 * 100.0);
	return val_duty;
}

static int read_fan_rpms(void)
{
	int raw_rpm = (read_ec(0xD0) << 8) + (read_ec(0xD1));
	return raw_rpm > 0 ? (2156220 / raw_rpm) : 0;
}

static int write_fan_duty(int duty_percentage)
{
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
