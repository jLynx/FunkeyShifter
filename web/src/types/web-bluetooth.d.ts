type BluetoothServiceUUID = string | number;
type BluetoothCharacteristicUUID = string | number;

type BluetoothRequestDeviceFilter = {
  name?: string;
  namePrefix?: string;
  services?: BluetoothServiceUUID[];
};

type BluetoothRequestDeviceOptions = {
  acceptAllDevices?: boolean;
  filters?: BluetoothRequestDeviceFilter[];
  optionalServices?: BluetoothServiceUUID[];
};

interface Bluetooth extends EventTarget {
  getAvailability?(): Promise<boolean>;
  requestDevice(options?: BluetoothRequestDeviceOptions): Promise<BluetoothDevice>;
}

interface BluetoothDeviceEventMap {
  gattserverdisconnected: Event;
}

interface BluetoothDevice extends EventTarget {
  readonly gatt?: BluetoothRemoteGATTServer;
  readonly id: string;
  readonly name?: string;

  addEventListener<K extends keyof BluetoothDeviceEventMap>(
    type: K,
    listener: (this: BluetoothDevice, ev: BluetoothDeviceEventMap[K]) => void,
    options?: boolean | AddEventListenerOptions,
  ): void;
  removeEventListener<K extends keyof BluetoothDeviceEventMap>(
    type: K,
    listener: (this: BluetoothDevice, ev: BluetoothDeviceEventMap[K]) => void,
    options?: boolean | EventListenerOptions,
  ): void;
}

interface BluetoothRemoteGATTServer {
  readonly connected: boolean;
  readonly device: BluetoothDevice;

  connect(): Promise<BluetoothRemoteGATTServer>;
  disconnect(): void;
  getPrimaryService(service: BluetoothServiceUUID): Promise<BluetoothRemoteGATTService>;
}

interface BluetoothRemoteGATTService {
  readonly device: BluetoothDevice;
  readonly isPrimary: boolean;
  readonly uuid: string;

  getCharacteristic(characteristic: BluetoothCharacteristicUUID): Promise<BluetoothRemoteGATTCharacteristic>;
}

interface BluetoothRemoteGATTCharacteristicEventMap {
  characteristicvaluechanged: Event;
}

interface BluetoothRemoteGATTCharacteristic extends EventTarget {
  readonly service: BluetoothRemoteGATTService;
  readonly uuid: string;
  readonly value?: DataView;

  addEventListener<K extends keyof BluetoothRemoteGATTCharacteristicEventMap>(
    type: K,
    listener: (this: BluetoothRemoteGATTCharacteristic, ev: BluetoothRemoteGATTCharacteristicEventMap[K]) => void,
    options?: boolean | AddEventListenerOptions,
  ): void;
  readValue(): Promise<DataView>;
  removeEventListener<K extends keyof BluetoothRemoteGATTCharacteristicEventMap>(
    type: K,
    listener: (this: BluetoothRemoteGATTCharacteristic, ev: BluetoothRemoteGATTCharacteristicEventMap[K]) => void,
    options?: boolean | EventListenerOptions,
  ): void;
  startNotifications(): Promise<BluetoothRemoteGATTCharacteristic>;
  stopNotifications(): Promise<BluetoothRemoteGATTCharacteristic>;
  writeValue(value: BufferSource): Promise<void>;
  writeValueWithResponse?(value: BufferSource): Promise<void>;
  writeValueWithoutResponse?(value: BufferSource): Promise<void>;
}

interface Navigator {
  readonly bluetooth: Bluetooth;
}
