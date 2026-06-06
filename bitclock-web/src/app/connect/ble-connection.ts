interface BluetoothConnection {
  device: BluetoothDevice;
  server: BluetoothRemoteGATTServer;
  service: BluetoothRemoteGATTService;
  homeAssistantService: BluetoothRemoteGATTService | null;
}

export type { BluetoothConnection };
