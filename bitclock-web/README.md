# bitclock web

Next.js web app for the Bitclock website and browser-based device configurator.

## Getting Started

First, run the development server:

```bash
npm run dev
```

Open [http://localhost:3000/connect](http://localhost:3000/connect) in Chrome to
configure a Bitclock over Web Bluetooth.

The connect page can configure:

- Wi-Fi credentials
- Display app selection
- Clock, weather, and temperature-unit preferences
- Home Assistant MQTT reporting

For Home Assistant MQTT, enter an MQTT broker URI such as
`mqtt://homeassistant.local:1883` and broker username/password credentials. Home
Assistant long-lived access tokens are not MQTT credentials.

## Build

```bash
npm run lint
npm run build
```

## Web Bluetooth Notes

Chrome may cache GATT service definitions. If the configurator cannot find a
new characteristic after a firmware update, remove the device from Chrome's
Bluetooth device permissions and from the OS Bluetooth settings, then reconnect.

Home Assistant MQTT configuration uses the dedicated service UUID
`1f44d9a0-4f71-4c9d-9ce8-5f1a950cc1dd` and characteristic UUID
`55940870-984a-4bbf-ae64-27b492ebd17e`.
