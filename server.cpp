#include <iostream>
#include <string>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <jsoncpp/json/json.h>
#include <map>
#include <mutex>
#include <thread>
#include <vector>
#include <atomic>

#define PORT 7878
#define BUFFER_SIZE 4096

std::mutex data_mutex;
std::mutex clients_mutex;
std::atomic<bool> server_running(true);

struct DeviceData {
    double temperature;
    double soil_moisture;
    double temp_threshold;
    double moisture_threshold;
    bool watering;
};

std::map<std::string, DeviceData> device_data_map;
std::map<int, std::pair<std::string, int>> connected_clients; // socket_fd -> (device_id, client_type)

enum ClientType {
    CLIENT_UNKNOWN = 0,
    CLIENT_STM32 = 1,
    CLIENT_PC = 2
};

std::string create_ack(const std::string& device_id, const std::string& status) {
    Json::Value root;
    root["command"] = "ack";
    root["device_id"] = device_id;
    root["status"] = status;
    
    Json::StreamWriterBuilder writer;
    return Json::writeString(writer, root);
}

std::string create_data_response(const std::string& device_id) {
    std::lock_guard<std::mutex> lock(data_mutex);
    if (device_data_map.find(device_id) == device_data_map.end()) {
        return create_ack(device_id, "device_not_found");
    }
    
    const auto& data = device_data_map[device_id];
    
    Json::Value root;
    root["command"] = "data_response";
    root["device_id"] = device_id;
    
    Json::Value data_obj;
    data_obj["temperature"] = data.temperature;
    data_obj["soil_moisture"] = data.soil_moisture;
    data_obj["temp_threshold"] = data.temp_threshold;
    data_obj["moisture_threshold"] = data.moisture_threshold;
    data_obj["watering"] = data.watering;
    
    root["data"] = data_obj;
    
    Json::StreamWriterBuilder writer;
    return Json::writeString(writer, root);
}

std::string create_update_threshold(const std::string& device_id, double temp_threshold, double moisture_threshold) {
    Json::Value root;
    root["command"] = "update_threshold";
    root["device_id"] = device_id;
    root["temp_threshold"] = temp_threshold;
    root["moisture_threshold"] = moisture_threshold;
    
    Json::StreamWriterBuilder writer;
    return Json::writeString(writer, root);
}

void broadcast_to_pc_clients(const std::string& device_id, const std::string& message) {
    std::lock_guard<std::mutex> lock(clients_mutex);
    for (const auto& client : connected_clients) {
        if (client.second.second == CLIENT_PC) {
            send(client.first, message.c_str(), message.size(), 0);
        }
    }
}

void handle_client(int client_socket) {
    char buffer[BUFFER_SIZE] = {0};
    
    while (server_running) {
        int valread = read(client_socket, buffer, BUFFER_SIZE - 1);
        
        if (valread <= 0) {
            std::cerr << "Client disconnected or error reading" << std::endl;
            break;
        }
        
        buffer[valread] = '\0';
        std::cout << "Received message: " << buffer << std::endl;
        
        Json::Value root;
        Json::CharReaderBuilder reader;
        std::string errors;
        std::istringstream iss(buffer);
        
        if (!Json::parseFromStream(reader, iss, &root, &errors)) {
            std::cerr << "Failed to parse JSON: " << errors << std::endl;
            continue;
        }
        
        std::string command = root["command"].asString();
        std::string device_id = root["device_id"].asString();
        std::string response;
        
        if (command == "upload") {
            // STM32上传数据
            {
                std::lock_guard<std::mutex> lock(data_mutex);
                DeviceData data;
                Json::Value data_obj = root["data"];
                data.temperature = data_obj["temperature"].asDouble();
                data.soil_moisture = data_obj["soil_moisture"].asDouble();
                data.temp_threshold = data_obj["temp_threshold"].asDouble();
                data.moisture_threshold = data_obj["moisture_threshold"].asDouble();
                data.watering = data_obj["watering"].asBool();
                
                device_data_map[device_id] = data;
            }
            
            // 标记为STM32客户端
            {
                std::lock_guard<std::mutex> lock(clients_mutex);
                connected_clients[client_socket] = {device_id, CLIENT_STM32};
            }
            
            response = create_ack(device_id, "success");
            std::cout << "Updated data for device: " << device_id << std::endl;
            
            // 广播给所有PC客户端
            broadcast_to_pc_clients(device_id, create_data_response(device_id));
        } else if (command == "get_data") {
            // PC请求数据
            {
                std::lock_guard<std::mutex> lock(clients_mutex);
                connected_clients[client_socket] = {device_id, CLIENT_PC};
            }
            response = create_data_response(device_id);
            std::cout << "Responding to data request for device: " << device_id << std::endl;
        } else if (command == "set_threshold") {
            // PC设置阈值
            double temp_threshold = root["temp_threshold"].asDouble();
            double moisture_threshold = root["moisture_threshold"].asDouble();
            
            {
                std::lock_guard<std::mutex> lock(data_mutex);
                if (device_data_map.find(device_id) != device_data_map.end()) {
                    device_data_map[device_id].temp_threshold = temp_threshold;
                    device_data_map[device_id].moisture_threshold = moisture_threshold;
                }
            }
            
            // 查找对应的STM32客户端并发送更新
            int stm32_socket = -1;
            {
                std::lock_guard<std::mutex> lock(clients_mutex);
                for (const auto& client : connected_clients) {
                    if (client.second.first == device_id && client.second.second == CLIENT_STM32) {
                        stm32_socket = client.first;
                        break;
                    }
                }
            }
            
            if (stm32_socket != -1) {
                std::string update_msg = create_update_threshold(device_id, temp_threshold, moisture_threshold);
                send(stm32_socket, update_msg.c_str(), update_msg.size(), 0);
                std::cout << "Forwarding threshold update to STM32 for device: " << device_id << std::endl;
            } else {
                response = create_ack(device_id, "device_not_connected");
                send(client_socket, response.c_str(), response.size(), 0);
                std::cerr << "STM32 device not connected: " << device_id << std::endl;
                continue;
            }
            
            // 等待STM32的确认
            char ack_buffer[BUFFER_SIZE] = {0};
            int ack_read = read(stm32_socket, ack_buffer, BUFFER_SIZE - 1);
            if (ack_read > 0) {
                ack_buffer[ack_read] = '\0';
                std::cout << "Received STM32 ACK: " << ack_buffer << std::endl;
                response = create_ack(device_id, "success");
            } else {
                response = create_ack(device_id, "device_not_responded");
            }
        } else if (command == "ack" && root["status"].asString() == "success") {
            // STM32确认阈值更新
            continue; // 已经在上面的set_threshold处理中回复PC
        } else {
            response = create_ack(device_id, "unknown_command");
            std::cerr << "Unknown command received: " << command << std::endl;
        }
        
        send(client_socket, response.c_str(), response.size(), 0);
        std::cout << "Sent response: " << response << std::endl;
    }
    
    // 客户端断开连接
    {
        std::lock_guard<std::mutex> lock(clients_mutex);
        connected_clients.erase(client_socket);
    }
    close(client_socket);
}

void accept_connections(int server_fd) {
    struct sockaddr_in address;
    int addrlen = sizeof(address);
    
    while (server_running) {
        int new_socket = accept(server_fd, (struct sockaddr *)&address, (socklen_t*)&addrlen);
        if (new_socket < 0) {
            if (server_running) {
                std::cerr << "Accept failed" << std::endl;
            }
            continue;
        }
        
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &address.sin_addr, client_ip, INET_ADDRSTRLEN);
        std::cout << "New connection from " << client_ip << ":" << ntohs(address.sin_port) << std::endl;
        
        std::thread client_thread(handle_client, new_socket);
        client_thread.detach();
    }
}

int main() {
    int server_fd;
    struct sockaddr_in address;
    int opt = 1;
    
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        std::cerr << "Socket creation error" << std::endl;
        return -1;
    }
    
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt))) {
        std::cerr << "Setsockopt error" << std::endl;
        return -1;
    }
    
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);
    
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        std::cerr << "Bind failed" << std::endl;
        return -1;
    }
    
    if (listen(server_fd, 10) < 0) {
        std::cerr << "Listen failed" << std::endl;
        return -1;
    }
    
    std::cout << "Server started on port " << PORT << std::endl;
    
    std::thread accept_thread(accept_connections, server_fd);
    
    // 简单的控制台命令处理
    std::string command;
    while (std::cin >> command) {
        if (command == "quit") {
            server_running = false;
            // 关闭所有客户端连接
            {
                std::lock_guard<std::mutex> lock(clients_mutex);
                for (const auto& client : connected_clients) {
                    close(client.first);
                }
                connected_clients.clear();
            }
            // 关闭服务器socket
            close(server_fd);
            break;
        } else if (command == "clients") {
            std::lock_guard<std::mutex> lock(clients_mutex);
            std::cout << "Connected clients (" << connected_clients.size() << "):" << std::endl;
            for (const auto& client : connected_clients) {
                std::cout << "Socket: " << client.first 
                          << ", Device ID: " << client.second.first 
                          << ", Type: " << (client.second.second == CLIENT_STM32 ? "STM32" : "PC") 
                          << std::endl;
            }
        } else if (command == "devices") {
            std::lock_guard<std::mutex> lock(data_mutex);
            std::cout << "Registered devices (" << device_data_map.size() << "):" << std::endl;
            for (const auto& device : device_data_map) {
                std::cout << "Device ID: " << device.first 
                          << ", Temp: " << device.second.temperature
                          << ", Moisture: " << device.second.soil_moisture
                          << std::endl;
            }
        } else {
            std::cout << "Unknown command. Available commands: quit, clients, devices" << std::endl;
        }
    }
    
    accept_thread.join();
    return 0;
}
