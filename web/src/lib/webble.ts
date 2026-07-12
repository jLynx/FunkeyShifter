export const FUNKEY_BLE_NAME = "FunkeyShifter";
export const FUNKEY_SERVICE_UUID = "8a8f9f85-0d1c-4e54-9f54-1f2e2a94d839";
export const FUNKEY_REPORT_CHARACTERISTIC_UUID = "8a8f9f86-0d1c-4e54-9f54-1f2e2a94d839";
export const FUNKEY_COMMAND_CHARACTERISTIC_UUID = "8a8f9f87-0d1c-4e54-9f54-1f2e2a94d839";
export const FUNKEY_PHYSICAL_REPORT_CHARACTERISTIC_UUID = "8a8f9f88-0d1c-4e54-9f54-1f2e2a94d839";

const CMD_SET_REPORT = 0x02;
const CMD_REMOVE = 0x03;
const CMD_USE_PHYSICAL = 0x04;
const REPORT_LEN = 8;
const PHYSICAL_CONTACT_ISSUE_REPORT = new Uint8Array([0xff, 0xff, 0xff, 0xf1, 0x00, 0x00, 0x00, 0x01]);

export type FunkeyBleConnection = {
  device: BluetoothDevice;
  server: BluetoothRemoteGATTServer;
  reportCharacteristic: BluetoothRemoteGATTCharacteristic;
  commandCharacteristic: BluetoothRemoteGATTCharacteristic;
  physicalReportCharacteristic: BluetoothRemoteGATTCharacteristic | null;
};

export type FunkeyBleDeviceInfo = {
  name: string;
  id: string;
};

export function isWebBluetoothAvailable(): boolean {
  return typeof navigator !== "undefined" && "bluetooth" in navigator;
}

export async function requestFunkeyBleDevice(): Promise<BluetoothDevice> {
  if (!isWebBluetoothAvailable()) {
    throw new Error("Web Bluetooth is unavailable in this browser.");
  }

  return navigator.bluetooth.requestDevice({
    filters: [{ services: [FUNKEY_SERVICE_UUID] }, { name: FUNKEY_BLE_NAME }],
    optionalServices: [FUNKEY_SERVICE_UUID],
  });
}

export async function connectFunkeyBleDevice(device: BluetoothDevice): Promise<FunkeyBleConnection> {
  if (!device.gatt) {
    throw new Error("Selected Bluetooth device does not expose GATT services.");
  }

  const server = await device.gatt.connect();
  const service = await server.getPrimaryService(FUNKEY_SERVICE_UUID);
  const reportCharacteristic = await service.getCharacteristic(FUNKEY_REPORT_CHARACTERISTIC_UUID);
  const commandCharacteristic = await service.getCharacteristic(FUNKEY_COMMAND_CHARACTERISTIC_UUID);
  const physicalReportCharacteristic = await optionalCharacteristic(service, FUNKEY_PHYSICAL_REPORT_CHARACTERISTIC_UUID);

  return {
    device,
    server,
    reportCharacteristic,
    commandCharacteristic,
    physicalReportCharacteristic,
  };
}

export async function readCurrentReport(connection: FunkeyBleConnection): Promise<Uint8Array> {
  const value = await connection.reportCharacteristic.readValue();
  return reportFromDataView(value);
}

export async function readPhysicalReport(connection: FunkeyBleConnection): Promise<Uint8Array | null> {
  if (!connection.physicalReportCharacteristic) {
    return null;
  }

  const value = await connection.physicalReportCharacteristic.readValue();
  return reportFromDataView(value);
}

export async function setCurrentReport(connection: FunkeyBleConnection, report: Uint8Array): Promise<void> {
  if (report.length !== REPORT_LEN) {
    throw new Error(`Report must be ${REPORT_LEN} bytes.`);
  }

  await writeCommand(connection, new Uint8Array([CMD_SET_REPORT, ...report]));
}

export async function removeCurrentReport(connection: FunkeyBleConnection): Promise<void> {
  await writeCommand(connection, new Uint8Array([CMD_REMOVE]));
}

export async function usePhysicalReport(connection: FunkeyBleConnection): Promise<void> {
  await writeCommand(connection, new Uint8Array([CMD_USE_PHYSICAL]));
}

export async function startReportNotifications(
  connection: FunkeyBleConnection,
  onReport: (report: Uint8Array) => void,
): Promise<() => void> {
  return startCharacteristicNotifications(connection.reportCharacteristic, onReport);
}

export async function startPhysicalReportNotifications(
  connection: FunkeyBleConnection,
  onReport: (report: Uint8Array) => void,
): Promise<(() => void) | null> {
  if (!connection.physicalReportCharacteristic) {
    return null;
  }

  return startCharacteristicNotifications(connection.physicalReportCharacteristic, onReport);
}

export function disconnectFunkeyBleDevice(connection: FunkeyBleConnection): void {
  if (connection.server.connected) {
    connection.server.disconnect();
  }
}

export function deviceInfo(device: BluetoothDevice): FunkeyBleDeviceInfo {
  return {
    name: device.name ?? FUNKEY_BLE_NAME,
    id: device.id,
  };
}

export function isPhysicalContactIssueReport(report: Uint8Array | null): boolean {
  if (!report || report.length !== REPORT_LEN) {
    return false;
  }

  return PHYSICAL_CONTACT_ISSUE_REPORT.every((value, index) => report[index] === value);
}

async function writeCommand(connection: FunkeyBleConnection, command: Uint8Array): Promise<void> {
  const buffer = toArrayBuffer(command);

  if (connection.commandCharacteristic.writeValueWithResponse) {
    await connection.commandCharacteristic.writeValueWithResponse(buffer);
    return;
  }

  await connection.commandCharacteristic.writeValue(buffer);
}

async function optionalCharacteristic(
  service: BluetoothRemoteGATTService,
  uuid: BluetoothCharacteristicUUID,
): Promise<BluetoothRemoteGATTCharacteristic | null> {
  try {
    return await service.getCharacteristic(uuid);
  } catch (error) {
    if (error instanceof DOMException && error.name === "NotFoundError") {
      return null;
    }

    throw error;
  }
}

async function startCharacteristicNotifications(
  characteristic: BluetoothRemoteGATTCharacteristic,
  onReport: (report: Uint8Array) => void,
): Promise<() => void> {
  const handleChange = (event: Event) => {
    const changedCharacteristic = event.target as BluetoothRemoteGATTCharacteristic | null;
    const value = changedCharacteristic?.value;
    if (!value) {
      return;
    }

    onReport(reportFromDataView(value));
  };

  characteristic.addEventListener("characteristicvaluechanged", handleChange);

  try {
    await characteristic.startNotifications();
  } catch (error) {
    characteristic.removeEventListener("characteristicvaluechanged", handleChange);
    throw error;
  }

  return () => {
    characteristic.removeEventListener("characteristicvaluechanged", handleChange);
    void characteristic.stopNotifications().catch(() => undefined);
  };
}

function reportFromDataView(value: DataView): Uint8Array {
  if (value.byteLength !== REPORT_LEN) {
    throw new Error(`Unexpected report length ${value.byteLength}.`);
  }

  return new Uint8Array(value.buffer.slice(value.byteOffset, value.byteOffset + REPORT_LEN));
}

function toArrayBuffer(bytes: Uint8Array): ArrayBuffer {
  const buffer = new ArrayBuffer(bytes.byteLength);
  new Uint8Array(buffer).set(bytes);
  return buffer;
}
