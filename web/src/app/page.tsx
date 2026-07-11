"use client";

import {
  Activity,
  AlertTriangle,
  BluetoothConnected,
  BluetoothSearching,
  Check,
  CircleOff,
  RefreshCw,
  Search,
  Send,
  Star,
  Unplug,
} from "lucide-react";
import { FormEvent, useEffect, useMemo, useRef, useState } from "react";
import {
  findVariantById,
  formatDisplayFunkeyId,
  formatFunkeyId,
  funkeyFamilies,
  funkeyVariants,
  FunkeyFamily,
  FunkeyVariant,
  idFromReport,
  parseFunkeyInput,
  rarityLabel,
  reportFromId,
} from "@/lib/funkeys";
import {
  connectFunkeyBleDevice,
  disconnectFunkeyBleDevice,
  FunkeyBleConnection,
  isWebBluetoothAvailable,
  readCurrentReport,
  removeCurrentReport,
  requestFunkeyBleDevice,
  setCurrentReport as writeCurrentReport,
  startReportNotifications,
} from "@/lib/webble";

type ConnectionState = "idle" | "connecting" | "connected" | "error";
type BusyId = number | "remove" | "custom" | "refresh" | null;

const sleep = (ms: number) => new Promise((resolve) => window.setTimeout(resolve, ms));
const PULSE_REMOVE = true;
const PULSE_DELAY = 250;
const FAVORITE_FUNKEYS_KEY = "funkey-shifter.favorite-ids";
const funkeyVariantById = new Map(funkeyVariants.map((variant) => [variant.id, variant]));

export default function Home() {
  const [connectionState, setConnectionState] = useState<ConnectionState>("idle");
  const [connection, setConnection] = useState<FunkeyBleConnection | null>(null);
  const [status, setStatus] = useState("Disconnected");
  const [currentReport, setCurrentReport] = useState<Uint8Array | null>(null);
  const [query, setQuery] = useState("");
  const [customValue, setCustomValue] = useState("");
  const [busyId, setBusyId] = useState<BusyId>(null);
  const [bluetoothReady, setBluetoothReady] = useState(false);
  const [favoriteIds, setFavoriteIds] = useState<number[]>([]);
  const [favoritesLoaded, setFavoritesLoaded] = useState(false);
  const stopNotificationsRef = useRef<(() => void) | null>(null);

  const currentId = currentReport ? idFromReport(currentReport) : null;
  const currentVariant = findVariantById(currentId);
  const isConnected = connectionState === "connected" && connection !== null && connection.server.connected;

  const filteredFamilies = useMemo(() => filterFamilies(query), [query]);
  const favoriteIdSet = useMemo(() => new Set(favoriteIds), [favoriteIds]);
  const favoriteVariants = useMemo(
    () => favoriteIds.flatMap((id) => {
      const variant = funkeyVariantById.get(id);
      return variant ? [variant] : [];
    }),
    [favoriteIds],
  );

  useEffect(() => {
    const supported = isWebBluetoothAvailable();
    setBluetoothReady(supported);

    if (!supported) {
      setConnectionState("error");
      setStatus("Web Bluetooth unavailable in this browser");
    }
  }, []);

  useEffect(() => {
    try {
      const rawFavorites = window.localStorage.getItem(FAVORITE_FUNKEYS_KEY);
      setFavoriteIds(rawFavorites ? normalizeFavoriteIds(JSON.parse(rawFavorites)) : []);
    } catch {
      setFavoriteIds([]);
    } finally {
      setFavoritesLoaded(true);
    }
  }, []);

  useEffect(() => {
    if (!favoritesLoaded) {
      return;
    }

    try {
      if (favoriteIds.length === 0) {
        window.localStorage.removeItem(FAVORITE_FUNKEYS_KEY);
      } else {
        window.localStorage.setItem(FAVORITE_FUNKEYS_KEY, JSON.stringify(favoriteIds));
      }
    } catch {
      // Favorites are a convenience only; selection should keep working if storage is unavailable.
    }
  }, [favoriteIds, favoritesLoaded]);

  useEffect(() => {
    if (!connection) {
      return;
    }

    const handleDisconnect = () => {
      stopReportNotifications();
      setConnection(null);
      setCurrentReport(null);
      setConnectionState("idle");
      setStatus("BLE disconnected");
    };

    connection.device.addEventListener("gattserverdisconnected", handleDisconnect);
    return () => {
      connection.device.removeEventListener("gattserverdisconnected", handleDisconnect);
    };
  }, [connection]);

  function stopReportNotifications() {
    const stopNotifications = stopNotificationsRef.current;
    stopNotificationsRef.current = null;
    stopNotifications?.();
  }

  async function attachDevice(selectedDevice: BluetoothDevice) {
    stopReportNotifications();
    setConnectionState("connecting");
    setStatus("Connecting over BLE...");

    let nextConnection: FunkeyBleConnection | null = null;
    let stopNotifications: (() => void) | null = null;

    try {
      nextConnection = await connectFunkeyBleDevice(selectedDevice);
      stopNotifications = await startReportNotifications(nextConnection, (report) => {
        setCurrentReport(report);
        setConnectionState("connected");
        setStatus("Current Funkey updated");
      });
      const report = await readCurrentReport(nextConnection);

      stopNotificationsRef.current = stopNotifications;
      setConnection(nextConnection);
      setCurrentReport(report);
      setConnectionState("connected");
      setStatus("Connected over BLE");
    } catch (error) {
      stopNotifications?.();
      if (nextConnection) {
        disconnectFunkeyBleDevice(nextConnection);
      }
      setConnection(null);
      setCurrentReport(null);
      setConnectionState("error");
      setStatus(errorMessage(error));
    }
  }

  async function connectWithChooser() {
    if (!bluetoothReady || connectionState === "connecting") {
      return;
    }

    setConnectionState("connecting");
    setStatus("Opening Bluetooth picker...");

    try {
      const selectedDevice = await requestFunkeyBleDevice();
      await attachDevice(selectedDevice);
    } catch (error) {
      setConnectionState("error");
      setStatus(errorMessage(error));
    }
  }

  async function disconnectDevice() {
    if (!connection) {
      return;
    }

    stopReportNotifications();
    disconnectFunkeyBleDevice(connection);
    setConnection(null);
    setCurrentReport(null);
    setConnectionState("idle");
    setStatus("Disconnected");
  }

  async function refreshStatus() {
    if (!connection) {
      return;
    }

    setBusyId("refresh");
    try {
      const report = await readCurrentReport(connection);
      setCurrentReport(report);
      setConnectionState("connected");
      setStatus("Status refreshed");
    } catch (error) {
      setConnectionState("error");
      setStatus(errorMessage(error));
    } finally {
      setBusyId(null);
    }
  }

  async function sendVariant(variant: FunkeyVariant) {
    if (!connection) {
      return;
    }

    setBusyId(variant.id);
    try {
      if (PULSE_REMOVE) {
        await removeCurrentReport(connection);
        await sleep(PULSE_DELAY);
      }

      const report = reportFromId(variant.id);
      await writeCurrentReport(connection, report);
      const readback = await readCurrentReport(connection);
      setCurrentReport(readback);
      setConnectionState("connected");
      setStatus(`Set ${variant.label}`);
    } catch (error) {
      setConnectionState("error");
      setStatus(errorMessage(error));
    } finally {
      setBusyId(null);
    }
  }

  async function removeFigure() {
    if (!connection) {
      return;
    }

    setBusyId("remove");
    try {
      await removeCurrentReport(connection);
      const readback = await readCurrentReport(connection);
      setCurrentReport(readback);
      setConnectionState("connected");
      setStatus("Removed");
    } catch (error) {
      setConnectionState("error");
      setStatus(errorMessage(error));
    } finally {
      setBusyId(null);
    }
  }

  async function sendCustom(event: FormEvent<HTMLFormElement>) {
    event.preventDefault();
    if (!connection) {
      return;
    }

    const id = parseFunkeyInput(customValue);
    if (id === null) {
      setConnectionState("error");
      setStatus("Enter a known name, decimal ID, hex ID, or full report");
      return;
    }

    setBusyId("custom");
    try {
      if (id === 0) {
        await removeCurrentReport(connection);
      } else {
        if (PULSE_REMOVE) {
          await removeCurrentReport(connection);
          await sleep(PULSE_DELAY);
        }

        await writeCurrentReport(connection, reportFromId(id));
      }

      const readback = await readCurrentReport(connection);
      setCurrentReport(readback);
      setConnectionState("connected");
      setStatus(id === 0 ? "Removed" : `Set ID ${formatDisplayFunkeyId(id)}`);
    } catch (error) {
      setConnectionState("error");
      setStatus(errorMessage(error));
    } finally {
      setBusyId(null);
    }
  }

  function toggleFavorite(variant: FunkeyVariant) {
    setFavoriteIds((currentIds) => {
      if (currentIds.includes(variant.id)) {
        return currentIds.filter((id) => id !== variant.id);
      }

      return [...currentIds, variant.id];
    });
  }

  return (
    <main className="app-shell">
      <header className="top-bar">
        <div className="brand">
          <div className="brand-mark" aria-hidden="true">
            <img src="/brand/ub.png" alt="" width={36} height={36} />
          </div>
          <div>
            <h1>FunkeyShifter</h1>
            <p>Pick a Funkey and switch instantly</p>
          </div>
        </div>

        <div className="connection-actions">
          <StatusPill state={connectionState} />
          {isConnected ? (
            <button className="secondary-button" type="button" onClick={disconnectDevice}>
              <Unplug size={17} />
              Disconnect
            </button>
          ) : (
            <button
              className="primary-button"
              type="button"
              onClick={connectWithChooser}
              disabled={!bluetoothReady || connectionState === "connecting"}
            >
              <BluetoothConnected size={17} />
              Connect BLE
            </button>
          )}
        </div>
      </header>

      <CurrentPanel
        className="mobile-current-dock"
        currentVariant={currentVariant}
        currentId={currentId}
        connectionState={connectionState}
        status={status}
        isConnected={isConnected}
        busyId={busyId}
        onRefresh={refreshStatus}
      />

      <div className="content-grid">
        <aside className="side-panel">
          <CurrentPanel
            className="desktop-current-panel"
            currentVariant={currentVariant}
            currentId={currentId}
            connectionState={connectionState}
            status={status}
            isConnected={isConnected}
            busyId={busyId}
            onRefresh={refreshStatus}
          />

          <section className="panel-section">
            <div className="section-title">
              <CircleOff size={18} />
              <h2>Figure</h2>
            </div>
            <button className="danger-button full-width" type="button" onClick={removeFigure} disabled={!isConnected || busyId !== null}>
              <CircleOff size={17} />
              Remove
            </button>
          </section>

          <section className="panel-section">
            <div className="section-title">
              <Send size={18} />
              <h2>Custom</h2>
            </div>
            <form className="custom-form" onSubmit={sendCustom}>
              <input
                value={customValue}
                onChange={(event) => setCustomValue(event.target.value)}
                placeholder="Webley or ID 92"
                spellCheck={false}
                aria-label="Custom Funkey name or ID"
              />
              <button className="primary-button" type="submit" disabled={!isConnected || busyId !== null}>
                <Send size={16} />
                Send
              </button>
            </form>
          </section>
        </aside>

        <section className="catalog-panel">
          <div className="catalog-toolbar">
            <div className="section-title">
              <Activity size={18} />
              <h2>Select Funkey</h2>
            </div>
            <label className="search-box">
              <Search size={17} />
              <input value={query} onChange={(event) => setQuery(event.target.value)} placeholder="Search" aria-label="Search Funkeys" />
            </label>
          </div>

          <FavoriteStrip
            variants={favoriteVariants}
            currentId={currentId}
            busyId={busyId}
            disabled={!isConnected || busyId !== null}
            favoriteIds={favoriteIdSet}
            loaded={favoritesLoaded}
            onSelect={sendVariant}
            onToggleFavorite={toggleFavorite}
          />

          <div className="catalog-grid">
            {filteredFamilies.map((family) => (
              <FunkeyFamilyRow
                key={family.name}
                family={family}
                currentId={currentId}
                busyId={busyId}
                disabled={!isConnected || busyId !== null}
                favoriteIds={favoriteIdSet}
                onSelect={sendVariant}
                onToggleFavorite={toggleFavorite}
              />
            ))}
          </div>
        </section>
      </div>
    </main>
  );
}

function FavoriteStrip({
  variants,
  currentId,
  busyId,
  disabled,
  favoriteIds,
  loaded,
  onSelect,
  onToggleFavorite,
}: {
  variants: FunkeyVariant[];
  currentId: number | null;
  busyId: BusyId;
  disabled: boolean;
  favoriteIds: ReadonlySet<number>;
  loaded: boolean;
  onSelect: (variant: FunkeyVariant) => void;
  onToggleFavorite: (variant: FunkeyVariant) => void;
}) {
  return (
    <section className="favorites-strip" aria-label="Favorite Funkeys">
      <div className="section-title">
        <Star size={18} />
        <h2>Favorites</h2>
      </div>

      {variants.length > 0 ? (
        <div className="favorite-grid">
          {variants.map((variant) => (
            <FunkeyVariantTile
              key={`favorite-${variant.id}`}
              variant={variant}
              currentId={currentId}
              busyId={busyId}
              disabled={disabled}
              isFavorite={favoriteIds.has(variant.id)}
              compact
              onSelect={onSelect}
              onToggleFavorite={onToggleFavorite}
            />
          ))}
        </div>
      ) : loaded ? (
        <p className="empty-favorites">No favorites yet</p>
      ) : null}
    </section>
  );
}

function CurrentPanel({
  className = "",
  currentVariant,
  currentId,
  connectionState,
  status,
  isConnected,
  busyId,
  onRefresh,
}: {
  className?: string;
  currentVariant: FunkeyVariant | undefined;
  currentId: number | null;
  connectionState: ConnectionState;
  status: string;
  isConnected: boolean;
  busyId: BusyId;
  onRefresh: () => void;
}) {
  return (
    <section className={`panel-section current-panel ${className}`.trim()} aria-live="polite">
      <div className="current-panel-header">
        <div className="section-title">
          <Activity size={18} />
          <h2>Current</h2>
        </div>
        <button
          className="icon-button"
          type="button"
          onClick={onRefresh}
          disabled={!isConnected || busyId !== null}
          title="Refresh status"
        >
          <RefreshCw size={18} className={busyId === "refresh" ? "spin" : undefined} />
        </button>
      </div>

      <div className="current-status">
        {currentVariant?.imagePath ? (
          <img
            className="current-funkey-thumb"
            src={currentVariant.imagePath}
            alt={`${currentVariant.label} character`}
            width={55}
            height={62}
          />
        ) : (
          <span className="current-funkey-thumb current-funkey-thumb-placeholder" aria-hidden="true" />
        )}
        <div className="current-status-copy">
          <strong>{currentVariant ? currentVariant.label : currentId === 0 ? "Removed" : "Unknown"}</strong>
          <small>
            {currentId === null ? "No Funkey selected" : currentId === 0 ? "No figure present" : `ID ${formatDisplayFunkeyId(currentId)}`}
          </small>
          <small className={`current-status-message ${connectionState}`}>{status}</small>
        </div>
      </div>
    </section>
  );
}

function StatusPill({ state }: { state: ConnectionState }) {
  const label = state === "connected" ? "Connected" : state === "connecting" ? "Pairing" : state === "error" ? "Attention" : "Offline";
  const Icon = state === "connected" ? BluetoothConnected : state === "connecting" ? BluetoothSearching : state === "error" ? AlertTriangle : Activity;

  return (
    <span className={`status-pill ${state}`}>
      <Icon size={15} />
      {label}
    </span>
  );
}

function FunkeyFamilyRow({
  family,
  currentId,
  busyId,
  disabled,
  favoriteIds,
  onSelect,
  onToggleFavorite,
}: {
  family: FunkeyFamily;
  currentId: number | null;
  busyId: BusyId;
  disabled: boolean;
  favoriteIds: ReadonlySet<number>;
  onSelect: (variant: FunkeyVariant) => void;
  onToggleFavorite: (variant: FunkeyVariant) => void;
}) {
  return (
    <article className="funkey-row">
      <div className="funkey-name">
        <strong>{family.name}</strong>
        <span className="funkey-series" title={`XML series ${family.seriesCode}`}>
          {family.series}
        </span>
      </div>
      <div className="variant-actions">
        {family.variants.map((variant) => (
          <FunkeyVariantTile
            key={`${variant.name}-${variant.rarity}`}
            variant={variant}
            currentId={currentId}
            busyId={busyId}
            disabled={disabled}
            isFavorite={favoriteIds.has(variant.id)}
            onSelect={onSelect}
            onToggleFavorite={onToggleFavorite}
          />
        ))}
      </div>
    </article>
  );
}

function FunkeyVariantTile({
  variant,
  currentId,
  busyId,
  disabled,
  isFavorite,
  compact = false,
  onSelect,
  onToggleFavorite,
}: {
  variant: FunkeyVariant;
  currentId: number | null;
  busyId: BusyId;
  disabled: boolean;
  isFavorite: boolean;
  compact?: boolean;
  onSelect: (variant: FunkeyVariant) => void;
  onToggleFavorite: (variant: FunkeyVariant) => void;
}) {
  const active = currentId === variant.id;
  const busy = busyId === variant.id;
  const favoriteLabel = isFavorite ? `Remove ${variant.label} from favorites` : `Add ${variant.label} to favorites`;

  return (
    <div className={`variant-tile ${compact ? "compact" : ""}`}>
      <button
        className={`variant-button ${variant.rarity} ${active ? "active" : ""}`}
        type="button"
        onClick={() => onSelect(variant)}
        disabled={disabled}
        title={`${variant.label} ID ${formatDisplayFunkeyId(variant.id)} (${formatFunkeyId(variant.id)})`}
      >
        <span>{rarityLabel(variant.rarity)}</span>
        <small className="variant-id">ID {formatDisplayFunkeyId(variant.id)}</small>
        {variant.imagePath ? (
          <img
            className="funkey-thumb"
            src={variant.imagePath}
            alt={`${variant.label} character`}
            width={55}
            height={62}
            loading="lazy"
          />
        ) : (
          <span className="funkey-thumb funkey-thumb-placeholder" aria-hidden="true" />
        )}
        {active ? (
          <Check size={14} className="variant-status-icon" />
        ) : busy ? (
          <RefreshCw size={14} className="variant-status-icon spin" />
        ) : null}
      </button>
      <button
        className={`favorite-button ${isFavorite ? "active" : ""}`}
        type="button"
        onClick={() => onToggleFavorite(variant)}
        aria-label={favoriteLabel}
        title={favoriteLabel}
      >
        <Star size={15} />
      </button>
    </div>
  );
}

function filterFamilies(query: string): FunkeyFamily[] {
  const normalized = query.trim().toLowerCase();
  if (normalized.length === 0) {
    return funkeyFamilies;
  }

  return funkeyFamilies.filter((family) => {
    const nameMatch = family.name.toLowerCase().includes(normalized);
    const seriesMatch =
      family.series.toLowerCase().includes(normalized) ||
      `series ${family.seriesCode}`.includes(normalized) ||
      family.seriesCode.includes(normalized);
    const variantMatch = family.variants.some((variant) => {
      return (
        variant.label.toLowerCase().includes(normalized) ||
        formatDisplayFunkeyId(variant.id).includes(normalized) ||
        formatFunkeyId(variant.id).toLowerCase().includes(normalized) ||
        variant.id.toString(16).toLowerCase().includes(normalized)
      );
    });
    return nameMatch || seriesMatch || variantMatch;
  });
}

function normalizeFavoriteIds(value: unknown): number[] {
  if (!Array.isArray(value)) {
    return [];
  }

  const seen = new Set<number>();
  const ids: number[] = [];

  for (const item of value) {
    const id = typeof item === "number" ? item : typeof item === "string" ? Number.parseInt(item, 10) : Number.NaN;
    if (!Number.isInteger(id) || seen.has(id) || !funkeyVariantById.has(id)) {
      continue;
    }

    seen.add(id);
    ids.push(id);
  }

  return ids;
}

function errorMessage(error: unknown): string {
  if (error instanceof DOMException && error.name === "NotFoundError") {
    return "Device selection cancelled";
  }

  if (error instanceof DOMException && error.name === "SecurityError") {
    return "Open this page from localhost or HTTPS in Chrome or Edge.";
  }

  if (error instanceof Error) {
    const message = error.message.toLowerCase();

    if (message.includes("bluetooth adapter not available") || message.includes("bluetooth adapter is not available")) {
      return "Bluetooth adapter unavailable. Turn on Bluetooth and try again.";
    }

    if (message.includes("gatt") || message.includes("networkerror")) {
      return "BLE connection failed. Replug or reset the ESP32 and try again.";
    }

    return error.message;
  }

  return "Bluetooth operation failed";
}
