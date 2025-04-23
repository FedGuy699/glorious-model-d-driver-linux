#include <libusb-1.0/libusb.h>
#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>

#include <iostream>
#include <thread>
#include <mutex>
#include <atomic>
#include <cstring>
#include <chrono>

using namespace std;

#define VENDOR_ID 0x258a
#define PRODUCT_ID 0x0033
#define INTERFACE 0

int accumulated_x = 0;
int accumulated_y = 0;
std::mutex move_mutex;

std::atomic<bool> left_button_down(false);
std::atomic<bool> right_button_down(false);
std::atomic<bool> movement_pending(false);

Display* display = nullptr;

void initX11() {
    display = XOpenDisplay(NULL);
    if (!display) {
        cerr << "Cannot open X11 display.\n";
        exit(1);
    }
}

void moveSmoothly(int dx, int dy) {
    if (dx != 0 || dy != 0) {
        XTestFakeRelativeMotionEvent(display, dx, dy, 0);
        XFlush(display);
    }
}

void movementProcessorThread() {
    while (true) {
        if (movement_pending.load()) {
            int dx = 0, dy = 0;
            {
                std::lock_guard<std::mutex> lock(move_mutex);
                dx = accumulated_x;
                dy = accumulated_y;
                accumulated_x = 0;
                accumulated_y = 0;
                movement_pending.store(false);
            }

            if (dx != 0 || dy != 0) {
                moveSmoothly(dx, dy);
            }
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }
}

void handleMouseClick(bool down, int button) {
    XTestFakeButtonEvent(display, button, down ? True : False, 0);
    XFlush(display);
}

void handleScroll(signed char scroll_data) {
    if (scroll_data == 0) return;
    int button = (scroll_data > 0) ? 4 : 5;
    for (int i = 0; i < abs(scroll_data); ++i) {
        XTestFakeButtonEvent(display, button, True, 0);
        XTestFakeButtonEvent(display, button, False, 0);
    }
    XFlush(display);
}

int main() {
    initX11();

    libusb_context* ctx = nullptr;
    libusb_device_handle* handle = nullptr;
    libusb_init(&ctx);

    handle = libusb_open_device_with_vid_pid(ctx, VENDOR_ID, PRODUCT_ID);
    if (!handle) {
        cerr << "Device not found.\n";
        return 1;
    }

    if (libusb_kernel_driver_active(handle, INTERFACE)) {
        libusb_detach_kernel_driver(handle, INTERFACE);
    }

    if (libusb_claim_interface(handle, INTERFACE) < 0) {
        cerr << "Cannot claim interface.\n";
        return 1;
    }

    unsigned char data[8] = {};
    int transferred;
    unsigned char prev_buttons = 0;

    std::thread movementThread(movementProcessorThread);
    movementThread.detach();

    while (true) {
        int res = libusb_interrupt_transfer(handle, 0x81, data, sizeof(data), &transferred, 5);
        if (res == 0 && transferred > 0) {
            int dx = (signed char)data[1];
            int dy = (signed char)data[3];
            signed char scroll = (signed char)data[5];
            unsigned char buttons = data[0];

            {
                std::lock_guard<std::mutex> lock(move_mutex);
                accumulated_x += dx;
                accumulated_y += dy;
                movement_pending.store(true);
            }

            if ((buttons & 0x01) && !(prev_buttons & 0x01)) handleMouseClick(true, 1);
            else if (!(buttons & 0x01) && (prev_buttons & 0x01)) handleMouseClick(false, 1);

            if ((buttons & 0x02) && !(prev_buttons & 0x02)) handleMouseClick(true, 3);
            else if (!(buttons & 0x02) && (prev_buttons & 0x02)) handleMouseClick(false, 3);

            if (scroll != 0) handleScroll(scroll);

            prev_buttons = buttons;

        } else if (res != LIBUSB_ERROR_TIMEOUT && res != 0) {
            cerr << "libusb error: " << libusb_error_name(res) << "\n";
            break;
        }
    }

    libusb_release_interface(handle, INTERFACE);
    libusb_close(handle);
    libusb_exit(ctx);

    if (display) XCloseDisplay(display);
    return 0;
}
