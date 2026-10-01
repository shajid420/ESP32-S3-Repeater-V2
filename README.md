ESP32-S3 Repeater SetupESP32-S3 Wi-Fi Repeater

A routed Wi-Fi repeater / range extender for ESP32-S3 built with the Arduino IDE.

The project uses the ESP32-S3 in WIFI_AP_STA mode:

STA interface connects to an existing Wi-Fi router.

AP interface creates a new Wi-Fi network for clients.

NAPT/NAT routes client traffic from the ESP32-S3 AP network through the upstream router.

A built-in web dashboard is used for setup, monitoring, Wi-Fi configuration and administration.

This is a routed/NAT repeater, not a transparent Layer-2 Wi-Fi bridge.

Features

ESP32-S3 Wi-Fi AP + STA mode

Routed Wi-Fi repeater using lwIP NAPT

Automatic upstream Wi-Fi reconnection

Reconnection backoff

Automatic AP subnet selection to avoid common subnet conflicts

Built-in DHCP/NAT support through the ESP32 Wi-Fi stack

First-boot captive setup portal

Professional web dashboard served directly from the ESP32

Dashboard login with HTTP Basic Authentication

Login brute-force throttling

Wi-Fi network scanner

Save up to 5 previously used upstream networks

Change repeater SSID and password

Select AP channel or use automatic channel selection

Change dashboard username and password

Dashboard branding, accent color and theme settings

System information page

RSSI, IP, connected clients, uptime and heap monitoring

Factory reset

BOOT-button factory reset

Serial diagnostic logging

Configuration stored in ESP32 NVS using Preferences

No external Arduino libraries required

Requirements

Hardware

ESP32-S3 development board

USB cable

2.4 GHz Wi-Fi network for the upstream connection

Software

Arduino IDE

ESP32 Arduino Core 3.0.0 or newer

ESP32-S3 board selected in Arduino IDE

The sketch uses libraries included with the ESP32 Arduino core:

WiFi
WebServer
DNSServer
Preferences

No additional library installation is required.

Arduino IDE Setup

1. Install ESP32 board support

In Arduino IDE, add the Espressif ESP32 Boards package through:

File → Preferences → Additional Boards Manager URLs

Then open:

Tools → Board → Boards Manager

Search for:

esp32

Install the ESP32 by Espressif Systems package.

Use version 3.x or newer.

2. Select the ESP32-S3 board

Select the ESP32-S3 board that matches your hardware.

For example:

Tools → Board → ESP32 Arduino → ESP32S3 Dev Module

If your board has a specific ESP32-S3 variant, select the matching board instead.

3. Upload

Open:

esp32s3_wifi_repeater.ino

Compile and upload it.

Open Serial Monitor:

115200 baud

First-Time Setup

After the first boot, the device enters SETUP mode.

Connect your phone or PC to:

ESP32-S3-Repeater-Setup

Default setup password:

repeater-setup

Then open:

http://192.168.4.1/

The setup portal allows you to configure:

Upstream Wi-Fi SSID

Upstream Wi-Fi password

Repeater SSID

Repeater password

Dashboard username

Dashboard password

After saving the configuration, the ESP32-S3 restarts automatically.

Normal Repeater Mode

After setup, connect your phone/laptop to the configured repeater SSID.

The ESP32-S3 creates its own subnet and routes client traffic through the upstream router.

Typical topology:

                 Internet
                    │
                    │
             Upstream Router
             192.168.1.1
                    │
                    │ Wi-Fi
                    │
             ┌──────────────┐
             │   ESP32-S3   │
             │  Wi-Fi STA   │
             │     NAT      │
             │  Wi-Fi AP    │
             └───────┬──────┘
                     │
              Repeater Wi-Fi
                     │
             ┌───────┴───────┐
             │               │
           Phone           Laptop

The AP network normally uses a different subnet, such as:

192.168.50.0/24

The firmware checks several possible AP subnets and chooses one that does not collide with the upstream network.

Dashboard

In repeater mode, open:

http://<ESP32-AP-IP>/

The dashboard requires the configured admin username and password.

The dashboard provides information such as:

Upstream connection state

Upstream SSID

Upstream IP

Repeater SSID

Repeater IP

RSSI

Wi-Fi channel

Connected clients

Uptime

Free heap

Minimum free heap

Reconnection count

NAT status

Error information

ESP32 chip/system information

Default Settings

Setting

Default

Firmware

1.1.0

Hostname

esp32s3-repeater

Setup SSID

ESP32-S3-Repeater-Setup

Setup password

repeater-setup

Default admin username

admin

Default panel name

ESP32-S3 Repeater

Default accent

#2f9bff

Maximum saved networks

5

Maximum configured AP clients

8

Setup AP IP

192.168.4.1

Default AP subnet

192.168.50.0/24

Serial baud rate

115200

Factory reset hold time

10 seconds

Change the default setup/admin credentials after installation.

Factory Reset

There are two ways to reset the configuration.

From Dashboard

Use:

Admin → Factory Reset

The device erases its saved configuration and restarts in setup mode.

BOOT Button

Press and hold the ESP32-S3 BOOT button for approximately:

10 seconds

The firmware then clears the saved configuration and restarts.

The code assumes the BOOT button is connected to GPIO 0, which is common on ESP32-S3 development boards. If your board uses another GPIO, change RESET_BUTTON_PIN in the sketch.

Serial Monitor

Open Serial Monitor at:

115200

The firmware uses tagged logs.

Examples:

[BOOT]
[WIFI]
[AP]
[NAT]
[WEB]
[CLIENT]
[ERROR]

Example:

[BOOT] ESP32-S3 Wi-Fi Repeater v1.1.0
[WIFI] Associated with upstream router
[AP] Access point interface started
[NAT] NAPT enabled

These logs are useful when troubleshooting connection problems.

Troubleshooting

Repeater AP appears but Internet does not work

Check:

ESP32-S3 is connected to the upstream router.

Upstream router has Internet access.

ESP32-S3 has received an STA IP address.

NAT status in the dashboard is active.

AP subnet is different from the upstream subnet.

For example, avoid:

Upstream: 192.168.1.0/24
AP:       192.168.1.0/24

The AP should use a different subnet, for example:

Upstream: 192.168.1.0/24
AP:       192.168.50.0/24

Wrong upstream password

The serial monitor should report an authentication/connection error.

Open the dashboard and update the upstream Wi-Fi credentials.

ESP32 repeatedly restarts

Check the serial log for:

BROWNOUT

If brownout is reported, use a stable USB power supply and cable.

Also monitor free heap from the dashboard/serial output.

Cannot open the setup page

Connect directly to:

ESP32-S3-Repeater-Setup

Then open:

http://192.168.4.1/

If your phone automatically switches back to mobile data, temporarily disable mobile data while completing setup.

Important Limitations

1. This is not a transparent Wi-Fi bridge

The design is:

Client → ESP32 AP → NAT → ESP32 STA → Router → Internet

It is not:

Client → transparent Layer-2 bridge → Router

Therefore, devices connected to the repeater are behind another NAT layer.

2. Double NAT

If the upstream router is already performing NAT, the network can become:

Client
  ↓
ESP32 NAT
  ↓
Upstream Router NAT
  ↓
Internet

Some applications that require inbound connections or special NAT behavior may not work exactly like they do on a normal access point.

3. One Wi-Fi radio

The ESP32-S3 uses one 2.4 GHz radio for both STA and AP operation.

This is not equivalent to a dedicated dual-radio commercial Wi-Fi repeater.

Throughput can be substantially lower than the upstream router's Wi-Fi speed, especially with multiple clients.

4. ESP32-S3 Wi-Fi capability

The project is intended for normal lightweight networking, browsing and IoT-style traffic.

It should not be expected to provide the throughput, range, client capacity or reliability of a commercial Wi-Fi router/repeater.

Security Notes

The dashboard uses HTTP Basic Authentication in repeater mode.

Additional protections include:

Password length validation

Constant-time password comparison for account changes

Login throttling after repeated failed attempts

CSRF-oriented custom XHR header for state-changing API requests

Security-related HTTP response headers

Configuration stored in ESP32 NVS

Saved network passwords are not returned by the web API

However, the dashboard uses HTTP, not HTTPS.

Do not expose the ESP32-S3 administration interface directly to an untrusted network or the public Internet.

Main API Endpoints

The firmware exposes several internal HTTP endpoints.

GET  /api/status
GET  /api/system
GET  /api/scan
GET  /api/saved

POST /api/setup
POST /api/wifi
POST /api/ap
POST /api/restart
POST /api/factory
POST /api/saved/use
POST /api/saved/del
POST /api/account
POST /api/brand

These endpoints are primarily used by the built-in dashboard.

Project Structure

The main project is currently contained in one Arduino sketch:

esp32s3_wifi_repeater/
└── esp32s3_wifi_repeater.ino

The web interface is embedded directly into the sketch using:

const char INDEX_HTML[] PROGMEM = R"HTML(... )HTML";

This means a separate web server or filesystem is not required.

Configuration Constants

Important settings can be changed near the top of the .ino file.

For example:

#define HOSTNAME            "esp32s3-repeater"
#define SETUP_AP_SSID       "ESP32-S3-Repeater-Setup"
#define SETUP_AP_PASS       "repeater-setup"
#define ADMIN_USER          "admin"
#define MAX_SAVED           5
#define RESET_BUTTON_PIN    0
#define MAX_AP_CLIENTS      8

Change these values before deployment if required.

License

Choose a license before publishing this project publicly.

For example:

MIT License

If this project contains code copied or adapted from another project, check the original project's license and retain the required copyright notices.

Disclaimer

This project is intended for legitimate networking, development, testing and educational use.

Only connect to and configure Wi-Fi networks that you own or are authorized to use.
