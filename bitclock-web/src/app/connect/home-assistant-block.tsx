import React, { useEffect, useState } from "react";

import {
  Alert,
  Box,
  Button,
  Group,
  PasswordInput,
  Switch,
  TextInput,
  Title,
} from "@mantine/core";
import { useForm } from "@mantine/form";
import { stringToArrayBuffer } from "@/libs/util";
import * as gatt from "@/libs/gatt";
import { BluetoothConnection } from "./ble-connection";

interface HomeAssistantBlockProps {
  bluetoothConnection: BluetoothConnection | null;
  homeAssistantConfig: gatt.HomeAssistantMqttConfig;
  setHomeAssistantConfig: (value: gatt.HomeAssistantMqttConfig) => void;
}

type HomeAssistantFormValues = gatt.HomeAssistantMqttConfig;

const containsSeparator = (value: string) =>
  value.includes("|") ? "Invalid character" : null;

export default function HomeAssistantBlock({
  bluetoothConnection,
  homeAssistantConfig,
  setHomeAssistantConfig,
}: HomeAssistantBlockProps) {
  const [error, setError] = useState<string | null>(null);
  const [saved, setSaved] = useState(false);
  const form = useForm<HomeAssistantFormValues>({
    initialValues: homeAssistantConfig,
    validate: {
      uri: (value, values) => {
        if (!values.enabled && value.length === 0) {
          return null;
        }
        if (value.length < 1 || value.length > 127) {
          return "Invalid broker URI";
        }
        if (!value.startsWith("mqtt://") && !value.startsWith("mqtts://")) {
          return "Use mqtt:// or mqtts://";
        }
        return containsSeparator(value);
      },
      username: (value) =>
        value.length > 63 ? "Invalid username" : containsSeparator(value),
      password: (value) =>
        value.length > 63 ? "Invalid password" : containsSeparator(value),
    },
  });

  useEffect(() => {
    form.setValues(homeAssistantConfig);
    form.resetDirty(homeAssistantConfig);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [homeAssistantConfig]);

  const sendHomeAssistantConfig = async (values: HomeAssistantFormValues) => {
    setError(null);
    setSaved(false);

    const service =
      bluetoothConnection?.homeAssistantService ?? bluetoothConnection?.service;

    if (!service) {
      setError("Not connected to a Bitclock GATT service.");
      return;
    }

    try {
      const characteristic = await service.getCharacteristic(
        gatt.CHR_HOME_ASSISTANT_MQTT_CONFIG_UUID,
      );

      await characteristic.writeValueWithResponse(
        stringToArrayBuffer(gatt.serializeHomeAssistantMqttConfig(values)),
      );
      setHomeAssistantConfig(values);
      form.resetDirty(values);
      setSaved(true);
    } catch (err) {
      setError(err instanceof Error ? err.message : `${err}`);
    }
  };

  return (
    <form onSubmit={form.onSubmit(sendHomeAssistantConfig)}>
      <Group>
        <Title order={3}>Home Assistant</Title>
      </Group>
      <Box maw={400}>
        <Switch
          mt="sm"
          label="MQTT reporting"
          {...form.getInputProps("enabled", { type: "checkbox" })}
        />
        <TextInput
          mt="md"
          label="Broker URI"
          placeholder="mqtt://homeassistant.local:1883"
          {...form.getInputProps("uri")}
        />
        <TextInput
          mt="md"
          label="Username"
          {...form.getInputProps("username")}
        />
        <PasswordInput
          mt="md"
          label="Password"
          {...form.getInputProps("password")}
        />
      </Box>
      <Button type="submit" mt="md">
        Update Home Assistant
      </Button>
      {error ? (
        <Alert color="red" mt="sm" title="Update failed">
          {error}
        </Alert>
      ) : null}
      {saved ? (
        <Alert color="green" mt="sm" title="Settings written">
          Home Assistant MQTT settings were written to the device.
        </Alert>
      ) : null}
    </form>
  );
}
