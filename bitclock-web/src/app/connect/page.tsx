"use client";

import React from "react";
import {
  Alert,
  Badge,
  Box,
  Button,
  Paper,
  Container,
  Divider,
  Group,
  List,
  Text,
} from "@mantine/core";
import { tzmap } from "@/libs/tz";
import { useState } from "react";
import Logo from "../logo";
import * as gatt from "@/libs/gatt";
import WifiBlock from "./wifi-block";
import { BluetoothConnection } from "./ble-connection";
import FirmwareBlock from "./firmware-block";
import WeatherBlock from "./weather-block";
import ClimateBlock from "./climate-block";
import ClockBlock from "./clock-block";
import AqiBlock from "./aqi-block";
import AppSelectionBlock from "./app-selection-block";
import HomeAssistantBlock from "./home-assistant-block";
import { IconBluetooth } from "@tabler/icons-react";

type GattAttributeParser<T> = {
  attributeId: number | string;
  parse: (value: DataView) => T;
  setter: (value: T) => void;
};

type GattReadResult = {
  attributeId: string;
  label: string;
  ok: boolean;
  phase: "read" | "notify" | "service";
  error?: string;
};

const OPTIONAL_GATT_READ_TIMEOUT_MS = 3000;
const REQUIRED_GATT_READ_RETRY_COUNT = 2;

const withTimeout = async <T,>(
  promise: Promise<T>,
  timeoutMs: number,
): Promise<T> => {
  let timeoutId: ReturnType<typeof setTimeout> | undefined;
  try {
    return await Promise.race([
      promise,
      new Promise<T>((_, reject) => {
        timeoutId = setTimeout(() => {
          reject(new Error(`Timed out after ${timeoutMs}ms`));
        }, timeoutMs);
      }),
    ]);
  } finally {
    if (timeoutId !== undefined) {
      clearTimeout(timeoutId);
    }
  }
};

const attributeIdToString = (attributeId: number | string): string =>
  typeof attributeId === "number" ? attributeId.toString(16) : attributeId;

const attributeIdToLabel = (attributeId: number | string): string => {
  switch (attributeId) {
    case gatt.CHR_TEMPERATURE_UUID:
      return "Temperature";
    case gatt.CHR_HUMIDITY_UUID:
      return "Humidity";
    case gatt.CHR_CO2_UUID:
      return "CO2";
    case gatt.CHR_VOC_INDEX_UUID:
      return "VOC index";
    case gatt.CHR_NOX_INDEX_UUID:
      return "NOx index";
    case gatt.CHR_WIFI_STATUS_UUID:
      return "Wi-Fi status";
    case gatt.CHR_TEMPERATURE_UNIT_UUID:
      return "Temperature unit";
    case gatt.CHR_CLOCK_FORMAT_UUID:
      return "Clock format";
    case gatt.CHR_WEATHER_PATH_UUID:
      return "Weather path";
    case gatt.CHR_TIMEZONE_UUID:
      return "Timezone";
    case gatt.CHR_APP_SELECTION_UUID:
      return "App selection";
    case gatt.CHR_FW_VERSION_UUID:
      return "Firmware version";
    case gatt.CHR_HOME_ASSISTANT_MQTT_CONFIG_UUID:
      return "Home Assistant MQTT config";
    default:
      return "Unknown";
  }
};

const parseTemperature = (value: DataView): number => {
  if (value.byteLength >= 4) {
    return value.getFloat32(0, true);
  }
  if (value.byteLength >= 2) {
    return value.getInt16(0, true) / 100;
  }
  if (value.byteLength >= 1) {
    return value.getInt8(0);
  }
  return 0;
};

const parseHumidity = (value: DataView): number => {
  if (value.byteLength >= 4) {
    return value.getFloat32(0, true);
  }
  if (value.byteLength >= 2) {
    return value.getUint16(0, true) / 100;
  }
  if (value.byteLength >= 1) {
    return value.getUint8(0);
  }
  return 0;
};

const parseUint16 = (value: DataView): number => {
  if (value.byteLength >= 2) {
    return value.getUint16(0, true);
  }
  if (value.byteLength >= 1) {
    return value.getUint8(0);
  }
  return 0;
};

const parseAirQualityIndex = (value: DataView): number => {
  if (value.byteLength >= 4) {
    return value.getInt32(0, true);
  }
  if (value.byteLength >= 2) {
    return value.getUint16(0, true);
  }
  if (value.byteLength >= 1) {
    return value.getUint8(0);
  }
  return 0;
};

const readGattAttribute = async <T,>(
  service: BluetoothRemoteGATTService,
  parser: GattAttributeParser<T>,
  timeoutMs?: number,
): Promise<GattReadResult> => {
  const attributeId = attributeIdToString(parser.attributeId);
  const label = attributeIdToLabel(parser.attributeId);
  try {
    const characteristicPromise = service.getCharacteristic(parser.attributeId);
    const characteristic =
      timeoutMs === undefined
        ? await characteristicPromise
        : await withTimeout(characteristicPromise, timeoutMs);
    const valuePromise = characteristic.readValue();
    const value =
      timeoutMs === undefined
        ? await valuePromise
        : await withTimeout(valuePromise, timeoutMs);
    if (value != undefined) {
      parser.setter(parser.parse(value));
    }
    return { attributeId, label, phase: "read", ok: true };
  } catch (err) {
    const error = err instanceof Error ? err.message : `${err}`;
    console.error(
      `Failed to read characteristic: ${attributeId}: ${label}: ${error}`,
    );
    return { attributeId, label, phase: "read", ok: false, error };
  }
};

const subscribeGattAttribute = async <T,>(
  service: BluetoothRemoteGATTService,
  parser: GattAttributeParser<T>,
): Promise<GattReadResult> => {
  const attributeId = attributeIdToString(parser.attributeId);
  const label = attributeIdToLabel(parser.attributeId);
  try {
    const characteristic = await service.getCharacteristic(parser.attributeId);
    (await characteristic.startNotifications()).addEventListener(
      "characteristicvaluechanged",
      (event) => {
        const target = event.target as BluetoothRemoteGATTCharacteristic;
        const value = target?.value;
        if (value != undefined) {
          parser.setter(parser.parse(value));
        }
      },
    );
    return { attributeId, label, phase: "notify", ok: true };
  } catch (err) {
    const error = err instanceof Error ? err.message : `${err}`;
    console.error(
      `Failed to observe characteristic: ${attributeId}: ${label}: ${error}`,
    );
    return { attributeId, label, phase: "notify", ok: false, error };
  }
};

export default function Home() {
  const [bluetoothConnection, setBluetoothConnection] =
    useState<BluetoothConnection | null>(null);
  const [firmwareVersion, setFirmwareVersion] = useState<string | null>(null);
  const [temperature, setTemperature] = useState(0);
  const [humidity, setHumidity] = useState(0);
  const [co2, setCo2] = useState(0);
  const [vocIndex, setVocIndex] = useState(0);
  const [noxIndex, setNoxIndex] = useState(0);
  const [temperatureUnit, setTemperatureUnit] = useState(0);
  const [timezone, setTimezone] = useState<string | null>(null);
  const [clockFormat, setClockFormat] = useState(0);
  const [weatherPath, setWeatherPath] = useState<string | null>(null);
  const [wifiStatus, setWifiStatus] = useState<gatt.WifiStatus | null>(null);
  const [appSelection, setAppSelection] = useState(0);
  const [homeAssistantConfig, setHomeAssistantConfig] =
    useState<gatt.HomeAssistantMqttConfig>({
      enabled: false,
      uri: "",
      username: "",
      password: "",
    });
  const [connectError, setConnectError] = useState<string | null>(null);
  const [gattReadResults, setGattReadResults] = useState<GattReadResult[]>([]);

  const decoder = new TextDecoder();

  const timezoneDecoder = (value: DataView): string | null => {
    let posixTimezone = decoder.decode(value);
    let matchedTzIndex = tzmap.findIndex(
      ([label, posix]) => posix == posixTimezone,
    );
    if (matchedTzIndex >= 0) {
      return matchedTzIndex.toString();
    }
    return null;
  };

  const attributeReadOnce: GattAttributeParser<any>[] = [
    {
      attributeId: gatt.CHR_TEMPERATURE_UNIT_UUID,
      parse: (value: DataView) => value.getUint8(0),
      setter: setTemperatureUnit,
    },
    {
      attributeId: gatt.CHR_CLOCK_FORMAT_UUID,
      parse: (value: DataView) => value.getUint8(0),
      setter: setClockFormat,
    },
    {
      attributeId: gatt.CHR_WEATHER_PATH_UUID,
      parse: (value: DataView) => decoder.decode(value),
      setter: setWeatherPath,
    },
    {
      attributeId: gatt.CHR_TIMEZONE_UUID,
      parse: timezoneDecoder,
      setter: setTimezone,
    },
    {
      attributeId: gatt.CHR_APP_SELECTION_UUID,
      parse: (value: DataView) => value.getUint8(0),
      setter: setAppSelection,
    },
    {
      attributeId: gatt.CHR_FW_VERSION_UUID,
      parse: (value: DataView) => decoder.decode(value),
      setter: setFirmwareVersion,
    },
  ];
  const homeAssistantConfigAttribute: GattAttributeParser<gatt.HomeAssistantMqttConfig> =
    {
      attributeId: gatt.CHR_HOME_ASSISTANT_MQTT_CONFIG_UUID,
      parse: (value: DataView) => gatt.parseHomeAssistantMqttConfig(value),
      setter: setHomeAssistantConfig,
    };
  const attributeObservers: GattAttributeParser<any>[] = [
    {
      attributeId: gatt.CHR_TEMPERATURE_UUID,
      parse: parseTemperature,
      setter: setTemperature,
    },
    {
      attributeId: gatt.CHR_HUMIDITY_UUID,
      parse: parseHumidity,
      setter: setHumidity,
    },
    {
      attributeId: gatt.CHR_CO2_UUID,
      parse: parseUint16,
      setter: setCo2,
    },
    {
      attributeId: gatt.CHR_VOC_INDEX_UUID,
      parse: parseAirQualityIndex,
      setter: setVocIndex,
    },
    {
      attributeId: gatt.CHR_NOX_INDEX_UUID,
      parse: parseAirQualityIndex,
      setter: setNoxIndex,
    },
    {
      attributeId: gatt.CHR_WIFI_STATUS_UUID,
      parse: (value: DataView) => gatt.parseWifiStatus(value),
      setter: setWifiStatus,
    },
  ];

  const connectToDevice = async () => {
    setConnectError(null);

    if (!navigator.bluetooth) {
      setConnectError("Web Bluetooth is not available in this browser.");
      return;
    }

    try {
      const device = await navigator.bluetooth.requestDevice({
        filters: [{ services: [gatt.HOME_ASSISTANT_SERVICE_UUID] }],
        optionalServices: [gatt.SERVICE_UUID],
      });
      device.addEventListener("gattserverdisconnected", () => {
        setBluetoothConnection(null);
      });
      if (!device?.gatt) {
        setConnectError("No Bluetooth GATT server was found for this device.");
        return;
      }
      setGattReadResults([]);
      const server = await device.gatt.connect();
      const service = await server.getPrimaryService(gatt.SERVICE_UUID);
      let homeAssistantService: BluetoothRemoteGATTService | null = null;
      try {
        homeAssistantService = await server.getPrimaryService(
          gatt.HOME_ASSISTANT_SERVICE_UUID,
        );
        setGattReadResults((current) => [
          ...current,
          {
            attributeId: gatt.HOME_ASSISTANT_SERVICE_UUID,
            label: "Home Assistant service",
            phase: "service",
            ok: true,
          },
        ]);
      } catch (err) {
        setGattReadResults((current) => [
          ...current,
          {
            attributeId: gatt.HOME_ASSISTANT_SERVICE_UUID,
            label: "Home Assistant service",
            phase: "service",
            ok: false,
            error: err instanceof Error ? err.message : `${err}`,
          },
        ]);
      }
      setBluetoothConnection({
        device: device,
        server: server,
        service: service,
        homeAssistantService,
      });

      // You need to read at least one value before calling startNoficiations()
      // in order to transition to an authorized session.
      for (let parser of [...attributeReadOnce, ...attributeObservers]) {
        let result = await readGattAttribute(service, parser);
        for (
          let retryCount = 0;
          !result.ok && retryCount < REQUIRED_GATT_READ_RETRY_COUNT;
          retryCount++
        ) {
          await new Promise((resolve) => setTimeout(resolve, 250));
          result = await readGattAttribute(service, parser);
        }
        setGattReadResults((current) => [...current, result]);
      }

      const homeAssistantConfigResult = await readGattAttribute(
        homeAssistantService ?? service,
        homeAssistantConfigAttribute,
        OPTIONAL_GATT_READ_TIMEOUT_MS,
      );
      setGattReadResults((current) => [...current, homeAssistantConfigResult]);

      for (let parser of attributeObservers) {
        const result = await subscribeGattAttribute(service, parser);
        setGattReadResults((current) => [...current, result]);
      }
    } catch (err) {
      setConnectError(err instanceof Error ? err.message : `${err}`);
    }
  };

  const disconnect = async () => {
    if (!bluetoothConnection?.server) {
      return;
    }

    await bluetoothConnection.server.disconnect();
    setBluetoothConnection(null);
  };

  return (
    <Container py="md">
      <Group justify="space-between" align="center">
        <Group>
          <Box style={{ width: 150 }}>
            <Logo />
          </Box>
          {bluetoothConnection ? (
            <Badge color="green" variant="outline">
              Connected
            </Badge>
          ) : (
            <Badge color="red" variant="outline">
              Disconnected
            </Badge>
          )}
        </Group>
        {bluetoothConnection ? (
          <Button size="md" onClick={disconnect} variant="outline">
            Disconnect
          </Button>
        ) : (
          <Button
            size="md"
            onClick={connectToDevice}
            variant="gradient"
            gradient={{ from: "indigo", to: "cyan", deg: 217 }}
            leftSection={<IconBluetooth size={20} />}
          >
            Connect to device
          </Button>
        )}
      </Group>
      {connectError ? (
        <Alert color="red" mt="md" title="Connection failed">
          {connectError}
        </Alert>
      ) : null}
      {!bluetoothConnection ? (
        <Box ta="center" mt="xl">
          <Paper shadow="xs" p="md" maw={600} ml="auto" mr="auto">
            <p>
              Make sure Bitclock is powered (via USB) and within Bluetooth
              range.
            </p>
            <p>Google Chrome for macOS/Windows required for Web Bluetooth.</p>
          </Paper>
        </Box>
      ) : (
        <Box pt="md">
          <Paper shadow="xs" p="sm" mb="md">
            <Group justify="space-between">
              <Text fw={500}>BLE reads</Text>
              <Text size="sm">
                {gattReadResults.filter((result) => result.ok).length}/
                {gattReadResults.length} OK
              </Text>
            </Group>
            {gattReadResults.some((result) => !result.ok) ? (
              <List size="sm" mt="xs">
                {gattReadResults
                  .filter((result) => !result.ok)
                  .map((result) => (
                    <List.Item key={`${result.phase}-${result.attributeId}`}>
                      {result.label} {result.phase}: {result.error}
                    </List.Item>
                  ))}
              </List>
            ) : null}
          </Paper>

          <AqiBlock
            temperature={temperature}
            humidity={humidity}
            co2={co2}
            vocIndex={vocIndex}
            noxIndex={noxIndex}
            temperatureUnit={temperatureUnit}
          />

          <Divider my="md" />

          <AppSelectionBlock
            bluetoothConnection={bluetoothConnection}
            appSelection={appSelection}
            setAppSelection={setAppSelection}
          />

          <Divider my="md" />

          <ClockBlock
            bluetoothConnection={bluetoothConnection}
            timezone={timezone}
            setTimezone={setTimezone}
            clockFormat={clockFormat}
            setClockFormat={setClockFormat}
          />

          <Divider my="md" />

          <ClimateBlock
            bluetoothConnection={bluetoothConnection}
            temperatureUnit={temperatureUnit}
            setTemperatureUnit={setTemperatureUnit}
          />

          <Divider my="md" />

          <WeatherBlock
            bluetoothConnection={bluetoothConnection}
            weatherPath={weatherPath}
            setWeatherPath={setWeatherPath}
          />

          <Divider my="md" />

          <WifiBlock
            bluetoothConnection={bluetoothConnection}
            wifiStatus={wifiStatus}
          />

          <Divider my="md" />

          <HomeAssistantBlock
            bluetoothConnection={bluetoothConnection}
            homeAssistantConfig={homeAssistantConfig}
            setHomeAssistantConfig={setHomeAssistantConfig}
          />

          <Divider my="md" />

          <FirmwareBlock
            bluetoothConnection={bluetoothConnection}
            firmwareVersion={firmwareVersion}
          />
        </Box>
      )}
    </Container>
  );
}
