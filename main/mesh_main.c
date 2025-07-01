/* Mesh Internal Communication Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <string.h>
#include <sys/poll.h>
#include <inttypes.h>
#include <inttypes.h>
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mesh.h"
#include "nvs_flash.h"
#include "mesh_netif.h"
#include "driver/gpio.h"
#include "freertos/semphr.h"
#include "ping/ping_sock.h"
#include <arpa/inet.h>
#include "protocol_examples_common.h"

#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gptimer.h"
#include "esp_system.h"
#include "esp_netif.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>

#include "sdkconfig.h"
#include <unistd.h>
#include <sys/socket.h>
#include <errno.h>
#include <netdb.h>            // struct addrinfo
/*******************************************************
 *                Macros
 *******************************************************/
#define EXAMPLE_BUTTON_GPIO     0
#define PORT                        CONFIG_EXAMPLE_PORT
#define SIGNALING_PORT              CONFIG_EXAMPLE_SIG_PORT
#define KEEPALIVE_IDLE              CONFIG_EXAMPLE_KEEPALIVE_IDLE
#define KEEPALIVE_INTERVAL          CONFIG_EXAMPLE_KEEPALIVE_INTERVAL
#define KEEPALIVE_COUNT             CONFIG_EXAMPLE_KEEPALIVE_COUNT
#define TCP_HOST_TYPE               CONFIG_TCP_HOST_TYPE
#define TIMER_DIVIDER         (16)  //  Hardware timer clock divider
#define TIMER_SCALE           (TIMER_BASE_CLK / TIMER_DIVIDER)  // convert counter value to seconds

// commands for internal mesh communication:
// <CMD> <PAYLOAD>, where CMD is one character, payload is variable dep. on command
#define CMD_KEYPRESSED 0x55
// CMD_KEYPRESSED: payload is always 6 bytes identifying address of node sending keypress event
#define CMD_ROUTE_TABLE 0x56
// CMD_KEYPRESSED: payload is a multiple of 6 listing addresses in a routing table
/*******************************************************
 *                Constants
 *******************************************************/
static const char *MESH_TAG = "mesh_main";
static const uint8_t MESH_ID[6] = { 0x77, 0x77, 0x77, 0x77, 0x77, 0x76};
static const int SERVER_PORT = 3333;
static const int SERVER_PORT2 = 3369;
static const int SIG_PORT = 3339;
static const int SIG_PORT2 = 3342;
static const char *TAG = "tcp_connection";
/*******************************************************
 *                Variable Definitions
 *******************************************************/
static char payload[120];
static char msg[1400];
static bool is_running = true;
static mesh_addr_t mesh_parent_addr;
static int mesh_layer = -1;
static esp_ip4_addr_t s_current_ip;
static mesh_addr_t s_route_table[CONFIG_MESH_ROUTE_TABLE_SIZE];
static int s_route_table_size = 0;
static SemaphoreHandle_t s_route_table_lock = NULL;
static uint8_t s_mesh_tx_payload[CONFIG_MESH_ROUTE_TABLE_SIZE*6+1];
static esp_netif_ip_info_t interface_ip_info = {0};   
static esp_ping_handle_t ping;
static bool test_time_max = 0;
static gptimer_handle_t gptimer = NULL;
/*******************************************************
 *                Function Declarations
 *******************************************************/
// interaction with public mqtt broker
void mqtt_app_start(void);
void mqtt_app_publish(char* topic, char *publish_string);
static void tcp_server_task(void *pvParameters);
static void tcp_client_task(void *pvParameters);
static void start_test(void *pvParameters);
esp_err_t initialize_ping(void);
esp_err_t esp_mesh_ping_task_start(void);
esp_err_t esp_mesh_tcp_task_start(void);
/*******************************************************
 *                Function Definitions
 *******************************************************/

static void example_timer_on_alarm_cb(void)
{
    // Stop timer the sooner the better
    gptimer_stop(gptimer);
    test_time_max = 1;
}

static bool initialise_timer(void) {
    gptimer_alarm_config_t alarm_config = {};  // Zero initialize
    gptimer_event_callbacks_t cbs = {};
    gptimer_config_t timer_config = {};

    // Set up the timer configuration
    timer_config.clk_src = GPTIMER_CLK_SRC_DEFAULT;
    timer_config.direction = GPTIMER_COUNT_UP;
    timer_config.resolution_hz = 1000*1000; // 1MHz, 1 tick = 1us
 //   timer_config.flags = 0; // Ensure default behavior

    // Create timer
    if (gptimer_new_timer(&timer_config, &gptimer) != ESP_OK) {
        printf("Failed to create timer\n");
        return 0;
    }

    // Set up the alarm configuration
    alarm_config.alarm_count = 12 * 1000 * 1000; // 120s
    alarm_config.reload_count = 0; // Start from 0
    alarm_config.flags.auto_reload_on_alarm = false; // No auto-reload

    // Register the event callback
    cbs.on_alarm = example_timer_on_alarm_cb;

    // Apply configurations
    if (gptimer_set_alarm_action(gptimer, &alarm_config) != ESP_OK) {
        printf("Failed to set timer alarm\n");
        return 0;
    }

    if (gptimer_register_event_callbacks(gptimer, &cbs, NULL) != ESP_OK) {
        printf("Failed to register timer callbacks\n");
        return 0;
    }

    if (gptimer_enable(gptimer) != ESP_OK) {
        printf("Failed to enable timer\n");
        return 0;
    }

    gptimer_start(gptimer);

    ESP_LOGI(TAG,"Timer initialized successfully!");
    return 1;
}

static void initialise_button(void)
{
    gpio_config_t io_conf = {0};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.pin_bit_mask = BIT64(EXAMPLE_BUTTON_GPIO);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = 1;
    io_conf.pull_down_en = 0;
    gpio_config(&io_conf);
}

static void initialise_test_button(void)
{
    gpio_config_t io_conf = {0};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.pin_bit_mask = BIT64(26);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = 1;
    io_conf.pull_down_en = 0;
    gpio_config(&io_conf);
}

void static recv_cb(mesh_addr_t *from, mesh_data_t *data)
{
    if (data->data[0] == CMD_ROUTE_TABLE) {
        int size =  data->size - 1;
        if (s_route_table_lock == NULL || size%6 != 0) {
            ESP_LOGE(MESH_TAG, "Error in receiving raw mesh data: Unexpected size");
            return;
        }
        xSemaphoreTake(s_route_table_lock, portMAX_DELAY);
        s_route_table_size = size / 6;
        for (int i=0; i < s_route_table_size; ++i) {
            ESP_LOGI(MESH_TAG, "Received Routing table [%d] "
                    MACSTR, i, MAC2STR(data->data + 6*i + 1));
        }
        memcpy(&s_route_table, data->data + 1, size);
        xSemaphoreGive(s_route_table_lock);
    } else if (data->data[0] == CMD_KEYPRESSED) {
        if (data->size != 7) {
            ESP_LOGE(MESH_TAG, "Error in receiving raw mesh data: Unexpected size");
            return;
        }
        ESP_LOGW(MESH_TAG, "Keypressed detected on node: "
                MACSTR, MAC2STR(data->data + 1));
    } else {
        ESP_LOGE(MESH_TAG, "Error in receiving raw mesh data: Unknown command");
    }
}

static void check_test_button(void *pvParameters)
{
    static bool old_level = true;
    bool new_level;
    bool run_check_button = true;
    char addr_str[128], addr_str2[128];
    char signal[1];
    int addr_family = (int)pvParameters;
    int ip_protocol = 0;
    int keepAlive = 1;
    int keepIdle = KEEPALIVE_IDLE;
    int keepInterval = KEEPALIVE_INTERVAL;
    int keepCount = KEEPALIVE_COUNT;
    struct sockaddr_storage dest_addr, dest_addr2;
    initialise_test_button();
    int sock = 0;
    int sock2 = 0;
    bool listen1ready = 0;
    bool listen2ready = 0;

    signal[0] = 0x10;

    if (addr_family == AF_INET) {
        struct sockaddr_in *dest_addr_ip4 = (struct sockaddr_in *)&dest_addr;
        dest_addr_ip4->sin_addr.s_addr = htonl(INADDR_ANY);
        dest_addr_ip4->sin_family = AF_INET;
        dest_addr_ip4->sin_port = htons(SIG_PORT);
        ip_protocol = IPPROTO_IP;
        struct sockaddr_in *dest_addr_ip4_2 = (struct sockaddr_in *)&dest_addr2;
        dest_addr_ip4_2->sin_addr.s_addr = htonl(INADDR_ANY);
        dest_addr_ip4_2->sin_family = AF_INET;
        dest_addr_ip4_2->sin_port = htons(SIG_PORT2);
    }

    int listen_sock = socket(addr_family, SOCK_STREAM, ip_protocol);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }
    int listen_sock2 = socket(addr_family, SOCK_STREAM, ip_protocol);
    if (listen_sock2 < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }
    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(listen_sock2, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#if defined(CONFIG_EXAMPLE_IPV4) && defined(CONFIG_EXAMPLE_IPV6)
    // Note that by default IPV6 binds to both protocols, it is must be disabled
    // if both protocols used at the same time (used in CI)
    setsockopt(listen_sock, IPPROTO_IPV6, IPV6_V6ONLY, &opt, sizeof(opt));
    setsockopt(listen_sock2, IPPROTO_IPV6, IPV6_V6ONLY, &opt, sizeof(opt));
#endif

    ESP_LOGI(TAG, "Signaling sockets created");

    int err = bind(listen_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (err != 0) {
        ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
        ESP_LOGE(TAG, "IPPROTO: %d", addr_family);
        goto CLEAN_UP;
    }
    ESP_LOGI(TAG, "Socket bound, port %d", SIG_PORT);
    err = bind(listen_sock2, (struct sockaddr *)&dest_addr2, sizeof(dest_addr2));
    if (err != 0) {
        ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
        ESP_LOGE(TAG, "IPPROTO: %d", addr_family);
        goto CLEAN_UP;
    }
    ESP_LOGI(TAG, "Socket bound, port %d", SIG_PORT2);

    err = listen(listen_sock, 1);
    if (err != 0) {
        ESP_LOGE(TAG, "Error occurred during listen: errno %d", errno);
        goto CLEAN_UP;
    }
    err = listen(listen_sock2, 1);
    if (err != 0) {
        ESP_LOGE(TAG, "Error occurred during listen: errno %d", errno);
        goto CLEAN_UP;
    }

    struct pollfd pfds[2];
    pfds[0].fd = listen_sock;          // Standard input
    pfds[0].events = POLLIN; // Tell me when ready to read
    pfds[1].fd = listen_sock2;          // Standard input
    pfds[1].events = POLLIN; // Tell me when ready to read

    while (1){
        ESP_LOGI(TAG, "Signaling Sockets listening");

        int num_events = poll(pfds, 2, -1);

	if (num_events == 0){
		ESP_LOGE(TAG, "No events happened here");
		//goto CLEAN_UP;
	}
	else {
	    int pollin_happened_socket1 = pfds[0].revents & POLLIN;
	    int pollin_happened_socket2 = pfds[1].revents & POLLIN;

            if (pollin_happened_socket1) {
       		 struct sockaddr_storage source_addr; // Large enough for both IPv4 or IPv6
        	 socklen_t addr_len = sizeof(source_addr);
        	 sock = accept(listen_sock, (struct sockaddr *)&source_addr, &addr_len);
        	 if (sock < 0) {
            	     ESP_LOGE(TAG, "Unable to accept connection port %d: errno %d", SIG_PORT, errno);
            	     break;
        	 }
        	 setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &keepAlive, sizeof(int));
        	 setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, &keepIdle, sizeof(int));
        	 setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &keepInterval, sizeof(int));
        	 setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, &keepCount, sizeof(int));
        	 // Convert ip address to string
        	 if (source_addr.ss_family == PF_INET) {
            		inet_ntoa_r(((struct sockaddr_in *)&source_addr)->sin_addr, addr_str, sizeof(addr_str) - 1);
        	 }
        	 ESP_LOGI(TAG, "Signaling socket accepted ip address: %s", addr_str);
		 listen1ready = 1;
	    }
	    if (pollin_happened_socket2) {
        	struct sockaddr_storage source_addr2; // Large enough for both IPv4 or IPv6
        	socklen_t addr_len2 = sizeof(source_addr2);
        	sock2 = accept(listen_sock2, (struct sockaddr *)&source_addr2, &addr_len2);
        	if (sock2 < 0) {
            	    ESP_LOGE(TAG, "Unable to accept connection: errno %d", errno);
            	    break;
        	}

        	// Set tcp keepalive option
        	setsockopt(sock2, SOL_SOCKET, SO_KEEPALIVE, &keepAlive, sizeof(int));
        	setsockopt(sock2, IPPROTO_TCP, TCP_KEEPIDLE, &keepIdle, sizeof(int));
        	setsockopt(sock2, IPPROTO_TCP, TCP_KEEPINTVL, &keepInterval, sizeof(int));
        	setsockopt(sock2, IPPROTO_TCP, TCP_KEEPCNT, &keepCount, sizeof(int));
        	if (source_addr2.ss_family == PF_INET) {
            	    inet_ntoa_r(((struct sockaddr_in *)&source_addr2)->sin_addr, addr_str2, sizeof(addr_str2) - 1);
        	}
        	ESP_LOGI(TAG, "Signaling socket accepted ip address: %s", addr_str2);
		listen2ready = 1;
	    }
	}
	if (listen1ready && listen2ready){  
        while (run_check_button) {
        new_level = gpio_get_level(26);
        if (!new_level && old_level) {
	    	// Send data
	    	int rply = send(sock, signal, sizeof(signal), 0);
	    	int rply2 = send(sock2, signal, sizeof(signal), 0);
	    	if (rply < 0 || rply2 < 0) {
		    ESP_LOGE(TAG, "send failed: errno %d", errno);
		   // break;
	    	}
	    	else {
		    ESP_LOGI(TAG, "Sent initiate test signal");
	    	}
            }
        old_level = new_level;
        vTaskDelay(50 / portTICK_PERIOD_MS);
	}
	}
    
        //shutdown(sock, 0);
        //close(sock);
        //shutdown(sock2, 0);
        //close(sock2);
    }
CLEAN_UP:
    ESP_LOGE(TAG, "Error, cleaning task check test button");
    close(listen_sock);
    close(listen_sock2);
    vTaskDelete(NULL);
}


static void check_button(void* args)
{
    static bool old_level = true;
    bool new_level;
    bool run_check_button = true;
    initialise_button();
    while (run_check_button) {
        new_level = gpio_get_level(EXAMPLE_BUTTON_GPIO);
        if (!new_level && old_level) {
	    esp_ping_start(ping);
        }
        old_level = new_level;
        vTaskDelay(50 / portTICK_PERIOD_MS);
    }
    vTaskDelete(NULL);
}

void mesh_event_handler(void *arg, esp_event_base_t event_base,
                        int32_t event_id, void *event_data)
{
    mesh_addr_t id = {0,};
    static uint8_t last_layer = 0;

    switch (event_id) {
    case MESH_EVENT_STARTED: {
        esp_mesh_get_id(&id);
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_MESH_STARTED>ID:"MACSTR"", MAC2STR(id.addr));
        mesh_layer = esp_mesh_get_layer();
    }
    break;
    case MESH_EVENT_STOPPED: {
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_STOPPED>");
        mesh_layer = esp_mesh_get_layer();
    }
    break;
    case MESH_EVENT_CHILD_CONNECTED: {
        mesh_event_child_connected_t *child_connected = (mesh_event_child_connected_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_CHILD_CONNECTED>aid:%d, "MACSTR"",
                 child_connected->aid,
                 MAC2STR(child_connected->mac));
    }
    break;
    case MESH_EVENT_CHILD_DISCONNECTED: {
        mesh_event_child_disconnected_t *child_disconnected = (mesh_event_child_disconnected_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_CHILD_DISCONNECTED>aid:%d, "MACSTR"",
                 child_disconnected->aid,
                 MAC2STR(child_disconnected->mac));
    }
    break;
    case MESH_EVENT_ROUTING_TABLE_ADD: {
        mesh_event_routing_table_change_t *routing_table = (mesh_event_routing_table_change_t *)event_data;
        ESP_LOGW(MESH_TAG, "<MESH_EVENT_ROUTING_TABLE_ADD>add %d, new:%d",
                 routing_table->rt_size_change,
                 routing_table->rt_size_new);
    }
    break;
    case MESH_EVENT_ROUTING_TABLE_REMOVE: {
        mesh_event_routing_table_change_t *routing_table = (mesh_event_routing_table_change_t *)event_data;
        ESP_LOGW(MESH_TAG, "<MESH_EVENT_ROUTING_TABLE_REMOVE>remove %d, new:%d",
                 routing_table->rt_size_change,
                 routing_table->rt_size_new);
    }
    break;
    case MESH_EVENT_NO_PARENT_FOUND: {
        mesh_event_no_parent_found_t *no_parent = (mesh_event_no_parent_found_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_NO_PARENT_FOUND>scan times:%d",
                 no_parent->scan_times);
    }
    /* TODO handler for the failure */
    break;
    case MESH_EVENT_PARENT_CONNECTED: {
        mesh_event_connected_t *connected = (mesh_event_connected_t *)event_data;
        esp_mesh_get_id(&id);
        mesh_layer = connected->self_layer;
        memcpy(&mesh_parent_addr.addr, connected->connected.bssid, 6);
        ESP_LOGI(MESH_TAG,
                 "<MESH_EVENT_PARENT_CONNECTED>layer:%d-->%d, parent:"MACSTR"%s, ID:"MACSTR"",
                 last_layer, mesh_layer, MAC2STR(mesh_parent_addr.addr),
                 esp_mesh_is_root() ? "<ROOT>" :
                 (mesh_layer == 2) ? "<layer2>" : "", MAC2STR(id.addr));
        last_layer = mesh_layer;
        mesh_netifs_start(esp_mesh_is_root());
    }
    break;
    case MESH_EVENT_PARENT_DISCONNECTED: {
        mesh_event_disconnected_t *disconnected = (mesh_event_disconnected_t *)event_data;
        ESP_LOGI(MESH_TAG,
                 "<MESH_EVENT_PARENT_DISCONNECTED>reason:%d",
                 disconnected->reason);
        mesh_layer = esp_mesh_get_layer();
        mesh_netifs_stop();
    }
    break;
    case MESH_EVENT_LAYER_CHANGE: {
        mesh_event_layer_change_t *layer_change = (mesh_event_layer_change_t *)event_data;
        mesh_layer = layer_change->new_layer;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_LAYER_CHANGE>layer:%d-->%d%s",
                 last_layer, mesh_layer,
                 esp_mesh_is_root() ? "<ROOT>" :
                 (mesh_layer == 2) ? "<layer2>" : "");
        last_layer = mesh_layer;
    }
    break;
    case MESH_EVENT_ROOT_ADDRESS: {
        mesh_event_root_address_t *root_addr = (mesh_event_root_address_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_ROOT_ADDRESS>root address:"MACSTR"",
                 MAC2STR(root_addr->addr));
    }
    break;
    case MESH_EVENT_VOTE_STARTED: {
        mesh_event_vote_started_t *vote_started = (mesh_event_vote_started_t *)event_data;
        ESP_LOGI(MESH_TAG,
                 "<MESH_EVENT_VOTE_STARTED>attempts:%d, reason:%d, rc_addr:"MACSTR"",
                 vote_started->attempts,
                 vote_started->reason,
                 MAC2STR(vote_started->rc_addr.addr));
    }
    break;
    case MESH_EVENT_VOTE_STOPPED: {
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_VOTE_STOPPED>");
        break;
    }
    case MESH_EVENT_ROOT_SWITCH_REQ: {
        mesh_event_root_switch_req_t *switch_req = (mesh_event_root_switch_req_t *)event_data;
        ESP_LOGI(MESH_TAG,
                 "<MESH_EVENT_ROOT_SWITCH_REQ>reason:%d, rc_addr:"MACSTR"",
                 switch_req->reason,
                 MAC2STR( switch_req->rc_addr.addr));
    }
    break;
    case MESH_EVENT_ROOT_SWITCH_ACK: {
        /* new root */
        mesh_layer = esp_mesh_get_layer();
        esp_mesh_get_parent_bssid(&mesh_parent_addr);
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_ROOT_SWITCH_ACK>layer:%d, parent:"MACSTR"", mesh_layer, MAC2STR(mesh_parent_addr.addr));
    }
    break;
    case MESH_EVENT_TODS_STATE: {
        mesh_event_toDS_state_t *toDs_state = (mesh_event_toDS_state_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_TODS_REACHABLE>state:%d", *toDs_state);
    }
    break;
    case MESH_EVENT_ROOT_FIXED: {
        mesh_event_root_fixed_t *root_fixed = (mesh_event_root_fixed_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_ROOT_FIXED>%s",
                 root_fixed->is_fixed ? "fixed" : "not fixed");
    }
    break;
    case MESH_EVENT_ROOT_ASKED_YIELD: {
        mesh_event_root_conflict_t *root_conflict = (mesh_event_root_conflict_t *)event_data;
        ESP_LOGI(MESH_TAG,
                 "<MESH_EVENT_ROOT_ASKED_YIELD>"MACSTR", rssi:%d, capacity:%d",
                 MAC2STR(root_conflict->addr),
                 root_conflict->rssi,
                 root_conflict->capacity);
    }
    break;
    case MESH_EVENT_CHANNEL_SWITCH: {
        mesh_event_channel_switch_t *channel_switch = (mesh_event_channel_switch_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_CHANNEL_SWITCH>new channel:%d", channel_switch->channel);
    }
    break;
    case MESH_EVENT_SCAN_DONE: {
        mesh_event_scan_done_t *scan_done = (mesh_event_scan_done_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_SCAN_DONE>number:%d",
                 scan_done->number);
    }
    break;
    case MESH_EVENT_NETWORK_STATE: {
        mesh_event_network_state_t *network_state = (mesh_event_network_state_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_NETWORK_STATE>is_rootless:%d",
                 network_state->is_rootless);
    }
    break;
    case MESH_EVENT_STOP_RECONNECTION: {
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_STOP_RECONNECTION>");
    }
    break;
    case MESH_EVENT_FIND_NETWORK: {
        mesh_event_find_network_t *find_network = (mesh_event_find_network_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_FIND_NETWORK>new channel:%d, router BSSID:"MACSTR"",
                 find_network->channel, MAC2STR(find_network->router_bssid));
    }
    break;
    case MESH_EVENT_ROUTER_SWITCH: {
        mesh_event_router_switch_t *router_switch = (mesh_event_router_switch_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_ROUTER_SWITCH>new router:%s, channel:%d, "MACSTR"",
                 router_switch->ssid, router_switch->channel, MAC2STR(router_switch->bssid));
    }
    break;
    default:
        ESP_LOGI(MESH_TAG, "unknown id:%" PRId32 "", event_id);
        break;
    }
}

void ip_event_handler(void *arg, esp_event_base_t event_base,
                      int32_t event_id, void *event_data)
{
    ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
    ESP_LOGI(MESH_TAG, "<IP_EVENT_STA_GOT_IP>IP:" IPSTR, IP2STR(&event->ip_info.ip));
    s_current_ip.addr = event->ip_info.ip.addr;
    esp_netif_get_ip_info(netif_sta, &interface_ip_info);
    ESP_LOGI(MESH_TAG, "NODE WIFI STA INTERFACE IP ADDRESS: "IPSTR"", IP2STR(&interface_ip_info.ip));
#if !CONFIG_MESH_USE_GLOBAL_DNS_IP
    esp_netif_t *netif = event->esp_netif;
    esp_netif_dns_info_t dns;
    ESP_ERROR_CHECK(esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns));
    mesh_netif_start_root_ap(esp_mesh_is_root(), dns.ip.u_addr.ip4.addr);
#endif
  //  esp_mesh_comm_mqtt_task_start();
    esp_netif_get_ip_info(netif_ap, &interface_ip_info);
    ESP_LOGI(MESH_TAG, "NODE WIFI AP INTERFACE IP ADDRESS: "IPSTR"", IP2STR(&interface_ip_info.ip));

#if TCP_HOST_TYPE == 0
    xTaskCreate(tcp_server_task, "tcp_server", 12288, (void*)AF_INET, 5, NULL);
#else
    xTaskCreate(start_test, "tcp_client", 4096, (void*)AF_INET, 5, NULL);
#endif
    initialize_ping();
    ESP_ERROR_CHECK(esp_mesh_ping_task_start());
#if CONFIG_MESH_NODE_ID == 0
    ESP_ERROR_CHECK(esp_mesh_tcp_task_start());
#endif
}

/* PING APP */

static void test_on_ping_success(esp_ping_handle_t hdl, void *args)
{
    // optionally, get callback arguments
    // const char* str = (const char*) args;
    // printf("%s\r\n", str); // "foo"
    uint8_t ttl;
    uint16_t seqno;
    uint32_t elapsed_time, recv_len;
    ip_addr_t target_addr;
    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seqno, sizeof(seqno));
    esp_ping_get_profile(hdl, ESP_PING_PROF_TTL, &ttl, sizeof(ttl));
    esp_ping_get_profile(hdl, ESP_PING_PROF_IPADDR, &target_addr, sizeof(target_addr));
    esp_ping_get_profile(hdl, ESP_PING_PROF_SIZE, &recv_len, sizeof(recv_len));
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &elapsed_time, sizeof(elapsed_time));
    printf("%" PRIu32 "bytes from %s icmp_seq=%d ttl=%d time=%" PRIu32 "ms\n",
           recv_len, inet_ntoa(target_addr.u_addr.ip4), seqno, ttl, elapsed_time);
}

static void test_on_ping_timeout(esp_ping_handle_t hdl, void *args)
{
    uint16_t seqno;
    ip_addr_t target_addr;
    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seqno, sizeof(seqno));
    esp_ping_get_profile(hdl, ESP_PING_PROF_IPADDR, &target_addr, sizeof(target_addr));
    printf("From %s icmp_seq=%d timeout\n", inet_ntoa(target_addr.u_addr.ip4), seqno);
}

static void test_on_ping_end(esp_ping_handle_t hdl, void *args)
{
    uint32_t transmitted;
    uint32_t received;
    uint32_t total_time_ms;

    esp_ping_get_profile(hdl, ESP_PING_PROF_REQUEST, &transmitted, sizeof(transmitted));
    esp_ping_get_profile(hdl, ESP_PING_PROF_REPLY, &received, sizeof(received));
    esp_ping_get_profile(hdl, ESP_PING_PROF_DURATION, &total_time_ms, sizeof(total_time_ms));
    printf("%" PRIu32 "packets transmitted, %" PRIu32 "received, time %" PRIu32 "ms\n", transmitted, received, total_time_ms);
}

esp_err_t initialize_ping(void)
{
    /* convert URL to IP address */
    ip_addr_t target_addr;
    esp_ip4_addr_t gw_addr;
    /*struct addrinfo hint;
    struct addrinfo *res = NULL;
    memset(&hint, 0, sizeof(hint));
    memset(&target_addr, 0, sizeof(target_addr));
    getaddrinfo("www.espressif.com", NULL, &hint, &res);
    struct in_addr addr4 = ((struct sockaddr_in *) (res->ai_addr))->sin_addr;
    inet_addr_to_ip4addr(ip_2_ip4(&target_addr), &addr4);
    freeaddrinfo(res);*/

#if CONFIG_MESH_NODE_ID == 0
    esp_netif_str_to_ip4("10.0.1.5", &gw_addr);
#else
    esp_netif_str_to_ip4("10.0.0.1", &gw_addr);
#endif

    memcpy((char *)&target_addr.u_addr.ip4, (char *)&gw_addr, sizeof(gw_addr));
    target_addr.type = IPADDR_TYPE_V4;

    esp_ping_config_t ping_config = ESP_PING_DEFAULT_CONFIG();
    ping_config.target_addr = target_addr;          // target IP address
    //ping_config.count = ESP_PING_COUNT_INFINITE;    // ping in infinite mode, esp_ping_stop can stop it
    ping_config.ttl = 1;

    /* set callback functions */
    esp_ping_callbacks_t cbs;
    cbs.on_ping_success = test_on_ping_success;
    cbs.on_ping_timeout = test_on_ping_timeout;
    cbs.on_ping_end = test_on_ping_end;
    cbs.cb_args = "foo";  // arguments that feeds to all callback functions, can be NULL
    //cbs.cb_args = eth_event_group;

    //esp_ping_handle_t ping;
    ESP_ERROR_CHECK(esp_ping_new_session(&ping_config, &cbs, &ping));

    return ESP_OK;
}

esp_err_t esp_mesh_ping_task_start(void)
{
    static bool is_ping_task_started = false;

    //s_route_table_lock = xSemaphoreCreateMutex();

    if (!is_ping_task_started) {
	ESP_LOGI(TAG,"Free heap size before task creation: %lu\n", esp_get_free_heap_size());
        xTaskCreate(check_button, "check button task", 3072, NULL, 5, NULL);
        is_ping_task_started = true;
    }
    return ESP_OK;
}


esp_err_t esp_mesh_tcp_task_start(void)
{
    static bool is_tcp_task_started = false;

    //s_route_table_lock = xSemaphoreCreateMutex();
    if (!is_tcp_task_started) {
        xTaskCreate(check_test_button, "check test button task", 4096, (void*)AF_INET, 5, NULL);
        is_tcp_task_started = true;
    }
    return ESP_OK;
}

static void tcp_server_task(void *pvParameters)
{
    char addr_str[128], addr_str2[128];
    int addr_family = (int)pvParameters;
    int ip_protocol = 0;
    int keepAlive = 1;
    int keepIdle = KEEPALIVE_IDLE;
    int keepInterval = KEEPALIVE_INTERVAL;
    int keepCount = KEEPALIVE_COUNT;
    struct sockaddr_storage dest_addr, dest_addr2;
    char rx_buffer[2600], rx_buffer2[2600];
    uint16_t server_packet_received_count = 0;
    uint16_t server_packet_sent_count = 0;
    bool packet_rx1 = 0;
    bool packet_rx2 = 0;
    bool start_messaging1 = 0;
    bool start_messaging2 = 0;
    int sock = 0;
    int sock2 = 0;
    int buffer1_rx_sum = 0;
    int buffer2_rx_sum = 0;
    uint16_t node_total_tx = 0;
    uint16_t node_total_rx = 0;
    uint16_t node2_total_tx = 0;
    uint16_t node2_total_rx = 0;
    uint32_t node_downlink_traffic = 0;
    uint32_t node2_downlink_traffic = 0;
    uint32_t server_uplink_bytes = 0;
    uint32_t server_uplink2_bytes = 0;



    for (int i = 0; i < 1400; i++) {
        msg[i] = 0xF7;  // fill packet with 1400 bytes
    }

    if (addr_family == AF_INET) {
        struct sockaddr_in *dest_addr_ip4 = (struct sockaddr_in *)&dest_addr;
        dest_addr_ip4->sin_addr.s_addr = htonl(INADDR_ANY);
        dest_addr_ip4->sin_family = AF_INET;
        dest_addr_ip4->sin_port = htons(SERVER_PORT);
        ip_protocol = IPPROTO_IP;
        struct sockaddr_in *dest_addr_ip4_2 = (struct sockaddr_in *)&dest_addr2;
        dest_addr_ip4_2->sin_addr.s_addr = htonl(INADDR_ANY);
        dest_addr_ip4_2->sin_family = AF_INET;
        dest_addr_ip4_2->sin_port = htons(SERVER_PORT2);
    }

    int listen_sock = socket(addr_family, SOCK_STREAM, ip_protocol);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }
    int listen_sock2 = socket(addr_family, SOCK_STREAM, ip_protocol);
    if (listen_sock2 < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    //int mss = 500;

    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(listen_sock2, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    //setsockopt(listen_sock, IPPROTO_TCP, TCP_MSS, &mss, sizeof(mss));
    //setsockopt(listen_sock2, IPPROTO_TCP, TCP_MSS, &mss, sizeof(mss));
#if defined(CONFIG_EXAMPLE_IPV4) && defined(CONFIG_EXAMPLE_IPV6)
    // Note that by default IPV6 binds to both protocols, it is must be disabled
    // if both protocols used at the same time (used in CI)
    setsockopt(listen_sock, IPPROTO_IPV6, IPV6_V6ONLY, &opt, sizeof(opt));
    setsockopt(listen_sock2, IPPROTO_IPV6, IPV6_V6ONLY, &opt, sizeof(opt));
#endif

    ESP_LOGI(TAG, "Sockets created");

    int err = bind(listen_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (err != 0) {
        ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
        ESP_LOGE(TAG, "IPPROTO: %d", addr_family);
        goto CLEAN_UP;
    }
    ESP_LOGI(TAG, "Socket bound, port %d", SERVER_PORT);
    
    err = bind(listen_sock2, (struct sockaddr *)&dest_addr2, sizeof(dest_addr2));
    if (err != 0) {
        ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
        ESP_LOGE(TAG, "IPPROTO: %d", addr_family);
        goto CLEAN_UP;
    }
    ESP_LOGI(TAG, "Socket bound, port %d", SERVER_PORT2);

    err = listen(listen_sock, 1);
    if (err != 0) {
        ESP_LOGE(TAG, "Error occurred during listen: errno %d", errno);
        goto CLEAN_UP;
    }
    
    err = listen(listen_sock2, 1);
    if (err != 0) {
        ESP_LOGE(TAG, "Error occurred during listen: errno %d", errno);
        goto CLEAN_UP;
    }
    
    ESP_LOGI(TAG, "Sockets listening");
    
    struct pollfd pfds[4];
    pfds[0].fd = listen_sock;          // Standard input
    pfds[0].events = POLLIN; // Tell me when ready to read
    pfds[1].fd = listen_sock2;          // Standard input
    pfds[1].events = POLLIN; // Tell me when ready to read
    pfds[2].fd = -1;
    pfds[3].fd = -1;

    while (1) {
        int num_events = poll(pfds, 4, 60000);
	ESP_LOGI(TAG, "Event loop starts!!!");	

	if (num_events <= 0){
		ESP_LOGE(TAG, "No events happened or poll error");
		goto CLEAN_UP;
		//continue;
	}
	else {
	    int pollin_happened_socket1 = pfds[0].revents & POLLIN;
	    int pollin_happened_socket2 = pfds[1].revents & POLLIN;

            if (pollin_happened_socket1) {
		if (!start_messaging1){
                    struct sockaddr_storage source_addr; // Large enough for both IPv4 or IPv6
        	    socklen_t addr_len = sizeof(source_addr);
        	    sock = accept(listen_sock, (struct sockaddr *)&source_addr, &addr_len);
        	    if (sock < 0){
    		        ESP_LOGE(TAG, "Unable to accept connection: errno %d", errno);
            	        break;
         	    }
		    pfds[2].fd = sock;
		    pfds[2].events = POLLIN;
        	    setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &keepAlive, sizeof(int));
        	    setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, &keepIdle, sizeof(int));
        	    setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &keepInterval, sizeof(int));
        	    setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, &keepCount, sizeof(int));
    	//	    setsockopt(sock, IPPROTO_TCP, TCP_MSS, &mss, sizeof(mss));
        	    if (source_addr.ss_family == PF_INET) {
            	        inet_ntoa_r(((struct sockaddr_in *)&source_addr)->sin_addr, addr_str, sizeof(addr_str) - 1);
        	    }
        	    ESP_LOGI(TAG, "Socket accepted ip address: %s", addr_str);
		    start_messaging1 = 1;
		}
            } 
	    if (pollin_happened_socket2){
	         if (!start_messaging2){
                     struct sockaddr_storage source_addr2; // Large enough for both IPv4 or IPv6
        	     socklen_t addr_len2 = sizeof(source_addr2);
        	     sock2 = accept(listen_sock2, (struct sockaddr *)&source_addr2, &addr_len2);
        	     if (sock2 < 0){
    		         ESP_LOGE(TAG, "Unable to accept connection: errno %d", errno);
            	         break;
         	     }
		     pfds[3].fd = sock2;
		     pfds[3].events = POLLIN;
     		     setsockopt(sock2, SOL_SOCKET, SO_KEEPALIVE, &keepAlive, sizeof(int));
       		     setsockopt(sock2, IPPROTO_TCP, TCP_KEEPIDLE, &keepIdle, sizeof(int));
        	     setsockopt(sock2, IPPROTO_TCP, TCP_KEEPINTVL, &keepInterval, sizeof(int));
       		     setsockopt(sock2, IPPROTO_TCP, TCP_KEEPCNT, &keepCount, sizeof(int));
    	//	     setsockopt(sock2, IPPROTO_TCP, TCP_MSS, &mss, sizeof(mss));
     	 	     if (source_addr2.ss_family == PF_INET) {
            		inet_ntoa_r(((struct sockaddr_in *)&source_addr2)->sin_addr, addr_str2, sizeof(addr_str2) - 1);
        	     }
        	     ESP_LOGI(TAG, "Socket accepted ip address: %s", addr_str2);
		     start_messaging2 = 1;
		 }
            }
	    int is_socket1_enabled = pfds[2].fd;
	    int is_socket2_enabled = pfds[3].fd;
	    int message_arrived_socket1 = pfds[2].revents & POLLIN;
	    int message_arrived_socket2 = pfds[3].revents & POLLIN;
	    if ((is_socket1_enabled != -1) && (message_arrived_socket1) && start_messaging1){
            	    int len = recv(sock, rx_buffer, sizeof(rx_buffer) - 1, 0);
            	    // Error occurred during receiving
            	    if (len <= 0) {
                	ESP_LOGE(TAG, "recv failed: errno %d", errno);
			close(pfds[2].fd);
       		        pfds[2].fd = -1; // Remove from poll
        		start_messaging1 = 0; // Reset state
			len = 0;
                	continue;
            	    }
	    	    else if (rx_buffer[0] == 0xF3) {
			ESP_LOGI(TAG, "Received end session flag. Restarting session ...");
			close(pfds[2].fd);
        		pfds[2].fd = -1;
        		//shutdown(sock, 0);
        		//close(sock);
			start_messaging1 = 0;
			len = 0;
	    	    }
	    	    else if (rx_buffer[0] == 0xFF) {    //if first byte is 0xFF, this is the info packet
			ESP_LOGI(TAG, "Received data from mesh node with size: %d", len);
			node_total_tx = rx_buffer[1] | (rx_buffer[2] << 8);
			node_total_rx = rx_buffer[3] | (rx_buffer[4] << 8);
			node_downlink_traffic = rx_buffer[5] | (rx_buffer[6] << 8) | (rx_buffer[7] << 16) | (rx_buffer[8] << 24);
			ESP_LOGI(TAG, "Mesh node total sent: %d, and total received: %d. Total received downlink traffic to this node: %" PRIu32 ".", node_total_tx, node_total_rx, node_downlink_traffic);
			close(pfds[2].fd);
        		pfds[2].fd = -1;
        		//shutdown(sock, 0);
        		//close(sock);
			start_messaging1 = 0;
			len = 0;
	    	    }
            	    else {
                	rx_buffer[len] = 0; // Null-terminate whatever we received and treat like a string
                	ESP_LOGI(TAG, "Received %d bytes", len);
                	//ESP_LOGI(TAG, "%s", rx_buffer);
			server_uplink_bytes = server_uplink_bytes + len;
			buffer1_rx_sum = buffer1_rx_sum + len;
			len = 0;
			if (buffer1_rx_sum >= 1296){
			    server_packet_received_count++;
			    buffer1_rx_sum = 0;
			    packet_rx1 = 1;
			}
	    	    }
		    if (start_messaging1 && packet_rx1){
	    	    	// Send data
	    	    	int rply = send(sock, msg, sizeof(msg), 0);
	    	    	if (rply < 0) {
		        	ESP_LOGE(TAG, "send failed: errno %d", errno);
				close(pfds[2].fd);
        			pfds[2].fd = -1;
        			//shutdown(sock, 0);
        			//close(sock);
				start_messaging1 = 0;
		       		// break;
				rply = 0;
				packet_rx1 = 0;
	    	    	}
	    	    	else {
		        	ESP_LOGI(TAG, "sent reply of %d bytes to host 1", rply);
		        	server_packet_sent_count = server_packet_sent_count + 1;
				rply = 0;
				packet_rx1 = 0;
	    	    	}
		    }
	    }
	    if ((is_socket2_enabled != -1) && (message_arrived_socket2) && start_messaging2){
		   	int len2 = recv(sock2, rx_buffer2, sizeof(rx_buffer2) - 1, 0);
            		// Error occurred during receiving
            	        if (len2 < 0) {
                	  ESP_LOGE(TAG, "recv failed: errno %d", errno);
			  close(pfds[3].fd);
			  pfds[3].fd = -1;
			  start_messaging2 = 0;
			  len2 = 0;
                	  continue;
            	        }
            		// Data received
	    	        else if (rx_buffer2[0] == 0xF3) {
			  ESP_LOGI(TAG, "Received end session flag. Restarting session ...");
        	 	  //shutdown(sock2, 0);
        		  //close(sock2);
			  close(pfds[3].fd);
			  pfds[3].fd = -1;
			  start_messaging2 = 0;
			  len2 = 0;
	    	        }
	    	        else if (rx_buffer2[0] == 0xFF) {    //if first byte is 0xFF, this is the info packet
			  ESP_LOGI(TAG, "Received data from mesh node with size: %d", len2);
			  node2_total_tx = rx_buffer2[1] | (rx_buffer2[2] << 8);
			  node2_total_rx = rx_buffer2[3] | (rx_buffer2[4] << 8);
			  node2_downlink_traffic = rx_buffer2[5] | (rx_buffer2[6] << 8) | (rx_buffer2[7] << 16) | (rx_buffer2[8] << 24);
			  ESP_LOGI(TAG, "Mesh node total sent: %d, and total received: %d. Total received downlink traffic to this node: %" PRIu32 ".", node2_total_tx, node2_total_rx, node2_downlink_traffic);
			  close(pfds[3].fd);
			  pfds[3].fd = -1;
			  start_messaging2 = 0;
			  len2 = 0;
	    	        }
			else{
                	    rx_buffer2[len2] = 0; // Null-terminate whatever we received and treat like a string
                	    ESP_LOGI(TAG, "Received %d bytes", len2);
			    server_uplink2_bytes = server_uplink2_bytes + len2;
			    buffer2_rx_sum = buffer2_rx_sum + len2;
			    len2 = 0;
			    if (buffer2_rx_sum >= 1296){
			        server_packet_received_count++;
				buffer2_rx_sum = 0;
				packet_rx2 = 1;
			    }
	    	        }
		     if (start_messaging2 && packet_rx2){
	    	     	int rply = send(sock2, msg, sizeof(msg), 0);
	    	     	if (rply < 0) {
		             ESP_LOGE(TAG, "send failed: errno %d", errno);
			     close(pfds[3].fd);
			     pfds[3].fd = -1;
			     start_messaging2 = 0;
			     rply = 0;
			     packet_rx2 = 0;
	    	     	}
	    	     	else {
		         	ESP_LOGI(TAG, "sent reply of %d bytes to host 2", rply);
		         	server_packet_sent_count = server_packet_sent_count + 1;
				rply = 0;
				packet_rx2 = 0;
	    	     	}
		     }
	    }
        }
    }

CLEAN_UP:
    ESP_LOGI(TAG, "Total packets sent from server: %d, and received: %d.", server_packet_sent_count, server_packet_received_count); 
    ESP_LOGI(TAG, "Total uplink traffic node 1: %" PRIu32 ", and uplink traffic node 2: %" PRIu32 ".", server_uplink_bytes, server_uplink2_bytes);
    close(listen_sock);
    close(listen_sock2);
    vTaskDelete(NULL);
}

static void tcp_client_task(void *pvParameters)
{
    char rx_buffer[130];
    int buffersize = 200;
    char host_ip[] = "10.0.0.1";   //tcp server mesh addr
    int addr_family = 0;
    int ip_protocol = 0;	
    static uint16_t total_packet_sent = 0;
    static uint16_t total_packet_received = 0;
    static uint32_t total_bytes_received = 0;
    bool info_sent = 0;
    payload[0] = 0x00;  //to make sure the first byte is not 0xFF

    for (int i = 1; i < 120; i++) {
        payload[i] = 0xF5;  // fill packet with 120 bytes
    }

    initialise_timer();

    while (!(test_time_max && info_sent)) {
#if defined(CONFIG_EXAMPLE_IPV4)
        struct sockaddr_in dest_addr;
        inet_pton(AF_INET, host_ip, &dest_addr.sin_addr);
        dest_addr.sin_family = AF_INET;
        dest_addr.sin_port = htons(PORT);
        addr_family = AF_INET;
        ip_protocol = IPPROTO_IP;
#elif defined(CONFIG_EXAMPLE_SOCKET_IP_INPUT_STDIN)
        struct sockaddr_storage dest_addr = { 0 };
        ESP_ERROR_CHECK(get_addr_from_stdin(PORT, SOCK_STREAM, &ip_protocol, &addr_family, &dest_addr));
#endif
        bool messages_per_session_flag = 0;
        uint16_t packet_sent_counter = 0;
        uint16_t packet_received_counter = 0;
	uint16_t bytes_received = 0;
	unsigned char info_payload[9];	
        int sock =  socket(addr_family, SOCK_STREAM, ip_protocol);
        if (sock < 0) {
            ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
            break;
        }
	//int mss = 500;
        setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &buffersize, sizeof(int));
    	//setsockopt(sock, IPPROTO_TCP, TCP_MSS, &mss, sizeof(mss));
        ESP_LOGI(TAG, "Socket created, connecting to %s:%d", host_ip, PORT);

        int err = connect(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
        if (err != 0) {
            ESP_LOGE(TAG, "Socket unable to connect: errno %d", errno);
            break;
        }
        ESP_LOGI(TAG, "Successfully connected");

        while (1) {
            int err = send(sock, payload, sizeof(payload), 0);
            if (err < 0) {
                ESP_LOGE(TAG, "Error occurred during sending: errno %d", errno);
                break;
            }
	    else {
	   	packet_sent_counter = packet_sent_counter + 1;
		ESP_LOGI(TAG, "Message sent number: %d", packet_sent_counter);
	   	if (packet_sent_counter >= 386){		//número de mensagens enviadas em uma sessão
			messages_per_session_flag = 1;
//		   	break;
	   	}
	    }
	//    int bytes_received_msg = 0;
	//    while (bytes_received_msg < 1400){
            int len = recv(sock, rx_buffer, sizeof(rx_buffer) - 1, 0);
            // Error occurred during receiving
            if (len < 0) {
                ESP_LOGE(TAG, "recv failed: errno %d", errno);
                break;
            }
            // Data received
            else {
                rx_buffer[len] = 0; // Null-terminate whatever we received and treat like a string
                ESP_LOGI(TAG, "Received %d bytes from %s:", len, host_ip);
                //ESP_LOGI(TAG, "%s", rx_buffer);
		bytes_received = bytes_received + len;
	//	bytes_received_msg = bytes_received_msg + len;
		len = 0;
		//if (bytes_received_msg >= 1400){
		    packet_received_counter++;
		//}
            }
	//    }

	    if (messages_per_session_flag == 1){
		    break;
	    }
	    else {
	   	vTaskDelay(1000 / portTICK_PERIOD_MS); //think time TCP
	    }
        }

   	total_packet_sent = total_packet_sent + packet_sent_counter;
    	total_packet_received = total_packet_received + packet_received_counter;
	total_bytes_received = total_bytes_received + bytes_received;

	if (test_time_max){
    	    info_payload[0] = 0xFF;  //indicate that this is the info packet
    	    info_payload[1] = (total_packet_sent & 0xFF);  // Low byte
    	    info_payload[2] = (total_packet_sent >> 8) & 0xFF;  // High byte

    	    // Pack uint16_2 into the next 2 bytes of payload
    	    info_payload[3] = (total_packet_received & 0xFF);  // Low byte
    	    info_payload[4] = (total_packet_received >> 8) & 0xFF;  // High byte

	    // Pack uint32 into 4 bytes
    	    info_payload[5] = (total_bytes_received & 0xFF);  // Low byte
    	    info_payload[6] = (total_bytes_received >> 8) & 0xFF;  // High byte
    	    info_payload[7] = (total_bytes_received >> 16) & 0xFF;  // Low byte
    	    info_payload[8] = (total_bytes_received >> 24) & 0xFF;  // High byte


    	    int inf = send(sock, info_payload, sizeof(info_payload), 0);
    	    if (inf < 0) {
        	ESP_LOGE(TAG, "Error occurred during sending of info to root: errno %d", errno);
    	    }
    	    else {
		ESP_LOGI(TAG, "Sent test info to root");
		info_sent = 1;
    	    }
	}
	else {
	    info_payload[0] = 0xF3;
    	    int stop = send(sock, info_payload, sizeof(info_payload), 0);
    	    if (stop < 0) {
        	ESP_LOGE(TAG, "Error occurred during sending of stop session to root: errno %d", errno);
    	    }
    	    else {
		ESP_LOGI(TAG, "Sent stop session to root");
		//close(sock);
    	    }

	}

        if (messages_per_session_flag == 1 || !test_time_max) {
            ESP_LOGE(TAG, "Shutting down socket and restarting...");
            //shutdown(sock, 0);
            //close(sock);
	    shutdown(sock, SHUT_WR); 
	    close(sock);
	    vTaskDelay(10000 / portTICK_PERIOD_MS); //time between sessions (idle time)
        }
    }

    ESP_LOGE(TAG, "Timer limit reached. Shutting down socket and closing task..");
    //shutdown(sock, 0);
    //close(sock);
    test_time_max = 0;
    vTaskDelete(NULL);
}


static void start_test(void *pvParameters)
{

    char rx_buffer[2];
    char host_ip[] = "10.0.0.1";   //tcp server mesh addr
    int addr_family = 0;
    int ip_protocol = 0;

    while (1) {
#if defined(CONFIG_EXAMPLE_IPV4)
        struct sockaddr_in dest_addr;
        inet_pton(AF_INET, host_ip, &dest_addr.sin_addr);
        dest_addr.sin_family = AF_INET;
        dest_addr.sin_port = htons(SIGNALING_PORT);
        addr_family = AF_INET;
        ip_protocol = IPPROTO_IP;
#elif defined(CONFIG_EXAMPLE_SOCKET_IP_INPUT_STDIN)
        struct sockaddr_storage dest_addr = { 0 };
        ESP_ERROR_CHECK(get_addr_from_stdin(SIGNALING_PORT, SOCK_STREAM, &ip_protocol, &addr_family, &dest_addr));
#endif
        int sock =  socket(addr_family, SOCK_STREAM, ip_protocol);
        if (sock < 0) {
            ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
            break;
        }
        ESP_LOGI(TAG, "Signaling Socket created, connecting to %s:%d", host_ip, SIGNALING_PORT);

        int err = connect(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
        if (err != 0) {
            ESP_LOGE(TAG, "Socket unable to connect: errno %d", errno);
            break;
        }
        ESP_LOGI(TAG, "Successfully connected");

	while (1){
            int len = recv(sock, rx_buffer, sizeof(rx_buffer) - 1, 0);
            // Error occurred during receiving
            if (len < 0) {
                ESP_LOGE(TAG, "recv failed: errno %d", errno);
                break;
            }
            // Data received
            else {
                rx_buffer[len] = 0; // Null-terminate whatever we received and treat like a string
                ESP_LOGI(TAG, "Received %d bytes from %s:", len, host_ip);
		if (rx_buffer[0] == 0x10){	
                    ESP_LOGI(TAG, "Initialize test");
    		    xTaskCreate(tcp_client_task, "tcp_client", 4096, (void*)AF_INET, 5, NULL);
		    break;
		}
		else{
		    ESP_LOGI(TAG, "Invalid flag to start test");
		}
            }
	    len = 0;
	}
        shutdown(sock, 0);
        close(sock);
        vTaskDelay(50000 / portTICK_PERIOD_MS);
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    /*  tcpip initialization */
    ESP_ERROR_CHECK(esp_netif_init());
    /*  event initialization */
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    /*  crete network interfaces for mesh (only station instance saved for further manipulation, soft AP instance ignored */
    ESP_ERROR_CHECK(mesh_netifs_init(recv_cb));
    /*  initialize built in blue led  */
    gpio_set_direction(GPIO_NUM_2, GPIO_MODE_OUTPUT);

    /*  wifi initialization */
    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&config));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip_event_handler, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_start());
    /*  mesh initialization */
    ESP_ERROR_CHECK(esp_mesh_init());
    ESP_ERROR_CHECK(esp_event_handler_register(MESH_EVENT, ESP_EVENT_ANY_ID, &mesh_event_handler, NULL));
    ESP_ERROR_CHECK(esp_mesh_set_max_layer(CONFIG_MESH_MAX_LAYER));
    ESP_ERROR_CHECK(esp_mesh_set_vote_percentage(1));
    ESP_ERROR_CHECK(esp_mesh_set_ap_assoc_expire(10));
    ESP_ERROR_CHECK(esp_mesh_fix_root(true));
    ESP_ERROR_CHECK(esp_mesh_allow_root_conflicts(false));
#if CONFIG_MESH_NODE_ID == 0
    ESP_ERROR_CHECK(esp_mesh_set_type(MESH_ROOT));
    gpio_set_level(GPIO_NUM_2, 1);
#elif CONFIG_MESH_NODE_ID == 2
    ESP_ERROR_CHECK(esp_mesh_set_type(MESH_LEAF));
#endif
    /* set blocking time of esp_mesh_send() to 30s, to prevent the esp_mesh_send() from permanently for some reason */
    ESP_ERROR_CHECK(esp_mesh_send_block_time(30000));
    mesh_cfg_t cfg = MESH_INIT_CONFIG_DEFAULT();
#if !MESH_IE_ENCRYPTED
    cfg.crypto_funcs = NULL;
#endif
    /* mesh ID */
    memcpy((uint8_t *) &cfg.mesh_id, MESH_ID, 6);
    /* router */
    cfg.channel = CONFIG_MESH_CHANNEL;
    cfg.router.ssid_len = strlen(CONFIG_MESH_ROUTER_SSID);
    memcpy((uint8_t *) &cfg.router.ssid, CONFIG_MESH_ROUTER_SSID, cfg.router.ssid_len);
    memcpy((uint8_t *) &cfg.router.password, CONFIG_MESH_ROUTER_PASSWD,
           strlen(CONFIG_MESH_ROUTER_PASSWD));
    /* mesh softAP */
    ESP_ERROR_CHECK(esp_mesh_set_ap_authmode(CONFIG_MESH_AP_AUTHMODE));
    cfg.mesh_ap.max_connection = CONFIG_MESH_AP_CONNECTIONS;
    cfg.mesh_ap.nonmesh_max_connection = CONFIG_MESH_NON_MESH_AP_CONNECTIONS;
    memcpy((uint8_t *) &cfg.mesh_ap.password, CONFIG_MESH_AP_PASSWD,
           strlen(CONFIG_MESH_AP_PASSWD));
    ESP_ERROR_CHECK(esp_mesh_set_config(&cfg));
    /* mesh start */
    ESP_ERROR_CHECK(esp_mesh_start());
    ESP_LOGI(MESH_TAG, "mesh starts successfully, heap:%" PRId32 ", %s",  esp_get_free_heap_size(),
             esp_mesh_is_root_fixed() ? "root fixed" : "root not fixed");
}
