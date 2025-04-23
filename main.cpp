#include <libusb-1.0/libusb.h>
#include <iostream>
#include <cstdlib>
#include <chrono>
#include <thread>
#include <mutex>
#include <vector>
#include <cmath>
#include <atomic>

using namespace std::chrono;

#define VENDOR_ID 0x258a
#define PRODUCT_ID 0x0033
#define INTERFACE 0

int accumulated_x = 0;
int accumulated_y = 0;
std::mutex move_mutex;

std::atomic<bool> left_button_down(false);
std::atomic<bool> right_button_down(false);

void moveSmoothly(int delta_x, int delta_y) {
    if (delta_x != 0 || delta_y != 0) {
        std::string cmd = "xdotool mousemove_relative -- ";
        cmd += std::to_string(delta_x);
        cmd += " ";
        cmd += std::to_string(delta_y);
        system(cmd.c_str());
    }
}

void processMovement() {
    while (true) {
        std::this_thread::sleep_for(milliseconds(0));
        int x_move, y_move;
        {
            std::lock_guard<std::mutex> lock(move_mutex);
            x_move = accumulated_x;
            y_move = accumulated_y;
            accumulated_x = 0;
            accumulated_y = 0;
        }
        moveSmoothly(x_move * 1, y_move * 1);
    }
}

void handleButtonClick(bool down, int button) {
    std::string action = down ? "mousedown" : "mouseup";
    std::string cmd = "xdotool " + action + " " + std::to_string(button);
    system(cmd.c_str());
}

void handleScroll(signed char scroll_data) {
    if (scroll_data != 0) {
        std::string cmd = "xdotool click ";
        if (scroll_data > 0) {
            cmd += "4";
        } else {
            cmd += "5";
        }
        for (int i = 0; i < std::abs(scroll_data); ++i) { 
            system(cmd.c_str());
            std::this_thread::sleep_for(microseconds(500)); 
        }
    }
}

int main() {
    libusb_context* ctx = nullptr;
    libusb_device_handle* handle = nullptr;
    libusb_init(&ctx);

    handle = libusb_open_device_with_vid_pid(ctx, VENDOR_ID, PRODUCT_ID);
    if (!handle) {
        std::cerr << "Mouse not found.\n";
        return 1;
    }

    if (libusb_kernel_driver_active(handle, INTERFACE) == 1) {
        libusb_detach_kernel_driver(handle, INTERFACE);
    }

    if (libusb_claim_interface(handle, INTERFACE) < 0) {
        std::cerr << "Cannot claim interface.\n";
        return 1;
    }

    unsigned char data[8];
    int transferred;
    unsigned char previous_buttons = 0;

    std::thread movementProcessor(processMovement);

    while (true) {
        int res = libusb_interrupt_transfer(handle, 0x81, data, sizeof(data), &transferred, 0);
        if (res == 0 && transferred > 0) {
            std::cout << "Data: ";
            for (int i = 0; i < 8; i++) {
                std::cout << (int)data[i] << " ";
            }
            std::cout << std::endl;

            int x_axis = (signed char)data[1];
            int y_axis = (signed char)data[3];
            unsigned char current_buttons = data[0];
            signed char scroll_wheel = (signed char)data[5];

            {
                std::lock_guard<std::mutex> lock(move_mutex);
                accumulated_x += x_axis;
                accumulated_y += y_axis;
            }

            if ((current_buttons & 0x01) && !(previous_buttons & 0x01)) {
                std::thread clickThread(handleButtonClick, true, 1);
                clickThread.detach();
                left_button_down = true;
            } else if (!(current_buttons & 0x01) && (previous_buttons & 0x01)) {
                std::thread clickThread(handleButtonClick, false, 1);
                clickThread.detach();
                left_button_down = false;
            }

            if ((current_buttons & 0x02) && !(previous_buttons & 0x02)) {
                std::thread clickThread(handleButtonClick, true, 3);
                clickThread.detach();
                right_button_down = true;
            } else if (!(current_buttons & 0x02) && (previous_buttons & 0x02)) {
                std::thread clickThread(handleButtonClick, false, 3);
                clickThread.detach();
                right_button_down = false;
            }

            if (scroll_wheel != 0) {
                std::thread scrollThread(handleScroll, scroll_wheel);
                scrollThread.detach();
            }

            previous_buttons = current_buttons;

        } else if (res == LIBUSB_ERROR_TIMEOUT) {
            continue;
        } else if (res != 0) {
            std::cerr << "USB Error: " << libusb_error_name(res) << "\n";
            break;
        }
    }

    movementProcessor.join();
    libusb_release_interface(handle, INTERFACE);
    libusb_close(handle);
    libusb_exit(ctx);
    return 0;
}