#include "esp_tcp.h"

#include <esp_log.h>
#include <unistd.h>
#include <cstring>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netdb.h>
#include <errno.h>

static const char *TAG = "EspTcp";

EspTcp::EspTcp() {
    event_group_ = xEventGroupCreate();
}

void EspTcp::JoinReceiveTask() {
    if (event_group_ == nullptr || !receive_task_started_) {
        return;
    }
    const EventBits_t bits = xEventGroupWaitBits(
        event_group_, ESP_TCP_EVENT_RECEIVE_TASK_EXIT, pdTRUE, pdFALSE,
        pdMS_TO_TICKS(10000));
    if (!(bits & ESP_TCP_EVENT_RECEIVE_TASK_EXIT)) {
        ESP_LOGE(TAG, "Failed to wait for receive task exit");
        return;
    }
    receive_task_started_ = false;
    receive_task_handle_ = nullptr;
}

EspTcp::~EspTcp() {
    DoDisconnect(true);

    if (event_group_ != nullptr && !receive_task_started_) {
        vEventGroupDelete(event_group_);
        event_group_ = nullptr;
    }
}

bool EspTcp::Connect(const std::string& host, int port) {
    // 确保先断开已有连接
    Disconnect();

    struct sockaddr_in server_addr;
    bzero(&server_addr, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    // host is domain
    struct hostent *server = gethostbyname(host.c_str());
    if (server == NULL) {
        last_error_ = h_errno;
        ESP_LOGE(TAG, "Failed to get host by name");
        return false;
    }
    memcpy(&server_addr.sin_addr, server->h_addr, server->h_length);

    tcp_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (tcp_fd_ < 0) {
        last_error_ = errno;
        ESP_LOGE(TAG, "Failed to create socket");
        return false;
    }

    int ret = connect(tcp_fd_, (struct sockaddr*)&server_addr, sizeof(server_addr));
    if (ret < 0) {
        last_error_ = errno;
        ESP_LOGE(TAG, "Failed to connect to %s:%d, code=0x%x", host.c_str(), port, last_error_);
        close(tcp_fd_);
        tcp_fd_ = -1;
        return false;
    }

    connected_ = true;

    xEventGroupClearBits(event_group_, ESP_TCP_EVENT_RECEIVE_TASK_EXIT);
    receive_task_started_ = false;
    if (xTaskCreate([](void* arg) {
            auto* tcp = static_cast<EspTcp*>(arg);
            EventGroupHandle_t events = tcp->event_group_;
            tcp->ReceiveTask();
            if (events != nullptr) {
                xEventGroupSetBits(events, ESP_TCP_EVENT_RECEIVE_TASK_EXIT);
            }
            vTaskDelete(nullptr);
        }, "tcp_receive", 4096, this, 1, &receive_task_handle_) != pdPASS) {
        receive_task_handle_ = nullptr;
        connected_ = false;
        shutdown(tcp_fd_, SHUT_RDWR);
        close(tcp_fd_);
        tcp_fd_ = -1;
        ESP_LOGE(TAG, "Failed to create receive task");
        return false;
    }
    receive_task_started_ = true;
    return true;
}

void EspTcp::Disconnect() {
    DoDisconnect(true);
}

void EspTcp::DoDisconnect(bool wait_for_task) {
    connected_ = false;

    if (tcp_fd_ != -1) {
        const int fd = tcp_fd_;
        tcp_fd_ = -1;
        shutdown(fd, SHUT_RDWR);
        close(fd);
    }

    // Always join the receive task on an active disconnect, even if the peer
    // already closed the socket and ReceiveTask cleared connected_. Otherwise
    // the destructor can delete event_group_ while the task still SetBits.
    if (wait_for_task) {
        JoinReceiveTask();
    }

    if (disconnect_callback_) {
        auto callback = disconnect_callback_;
        disconnect_callback_ = nullptr;
        callback();
    }
}

int EspTcp::Send(const std::string& data) {
    if (!connected_) {
        ESP_LOGE(TAG, "Not connected");
        return -1;
    }

    size_t total_sent = 0;
    size_t data_size = data.size();
    const char* data_ptr = data.data();

    while (total_sent < data_size) {
        int ret = send(tcp_fd_, data_ptr + total_sent, data_size - total_sent, 0);

        if (ret <= 0) {
            ESP_LOGE(TAG, "Send failed: ret=%d, errno=%d", ret, errno);
            return ret;
        }

        total_sent += ret;
    }

    return total_sent;
}

void EspTcp::ReceiveTask() {
    std::string data;
    while (connected_) {
        data.resize(1500);
        int ret = recv(tcp_fd_, data.data(), data.size(), 0);
        if (ret <= 0) {
            if (ret < 0) {
                ESP_LOGE(TAG, "TCP receive failed: %d", ret);
            }
            // 被动断开，不需要等待接收任务退出（当前就是接收任务）
            DoDisconnect(false);
            break;
        }

        if (stream_callback_) {
            data.resize(ret);
            stream_callback_(data);
        }
    }
}

int EspTcp::GetLastError() {
    return last_error_;
}

