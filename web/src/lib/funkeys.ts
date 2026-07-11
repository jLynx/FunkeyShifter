export type Rarity = "common" | "rare" | "ultraRare";

export type FunkeyVariant = {
  id: number;
  name: string;
  rarity: Rarity;
  label: string;
};

export type FunkeyFamily = {
  name: string;
  variants: FunkeyVariant[];
};

type CatalogRow = {
  name: string;
  common: number;
  rare?: number;
  ultraRare?: number;
};

const catalogRows: CatalogRow[] = [
  { name: "Scratch", common: 0x47, rare: 0x48, ultraRare: 0x49 },
  { name: "Lotus", common: 0x4a, rare: 0x4b, ultraRare: 0x4f },
  { name: "Drift", common: 0xaa, rare: 0xb7, ultraRare: 0xc4 },
  { name: "Waggs", common: 0xab, rare: 0xb8, ultraRare: 0xc5 },
  { name: "Dot", common: 0xfc, rare: 0xfd, ultraRare: 0x101 },
  { name: "Holler", common: 0xcd },
  { name: "Gabby", common: 0xcb },
  { name: "Henchman", common: 0xcf },
  { name: "Master Lox", common: 0xf7 },
  { name: "Mayor Sayso", common: 0xf8 },
  { name: "Stitch", common: 0x26, rare: 0x27, ultraRare: 0x28 },
  { name: "Deuce", common: 0x29, rare: 0x2a, ultraRare: 0x2e },
  { name: "Wasabi", common: 0x2f, rare: 0x30, ultraRare: 0x31 },
  { name: "Bones", common: 0x32, rare: 0x33, ultraRare: 0x34 },
  { name: "Xener", common: 0x35, rare: 0x39, ultraRare: 0x3a },
  { name: "Fallout", common: 0x3b, rare: 0x3c, ultraRare: 0x3d },
  { name: "Boggle", common: 0x3e, rare: 0x3f, ultraRare: 0x40 },
  { name: "Vroom", common: 0x44, rare: 0x45, ultraRare: 0x46 },
  { name: "Rom", common: 0xce },
  { name: "Glub", common: 0x14, rare: 0x18, ultraRare: 0x19 },
  { name: "Sprout", common: 0x1a, rare: 0x1b, ultraRare: 0x1c },
  { name: "Tiki", common: 0x1d, rare: 0x1e, ultraRare: 0x1f },
  { name: "Twinx", common: 0x23, rare: 0x24, ultraRare: 0x25 },
  { name: "Flurry", common: 0x50, rare: 0x51, ultraRare: 0x52 },
  { name: "Nibble", common: 0x53, rare: 0x54, ultraRare: 0x55 },
  { name: "Sol", common: 0x56, rare: 0x5a, ultraRare: 0x5b },
  { name: "Webley", common: 0x5c, rare: 0x5d, ultraRare: 0x5e },
  { name: "Jerry", common: 0xf1 },
  { name: "Pineapple King", common: 0xf2 },
  { name: "Native", common: 0xf6 },
  { name: "Rewind", common: 0xcc },
  { name: "Racer X", common: 0x5f, rare: 0x60, ultraRare: 0x61 },
  { name: "Trixie", common: 0x82, rare: 0x83, ultraRare: 0x87 },
  { name: "Cannonball Taylor", common: 0x88, rare: 0x89, ultraRare: 0x8a },
  { name: "Snake Oiler", common: 0x8b, rare: 0x8c, ultraRare: 0x8d },
  { name: "Speed Racer Pinball", common: 0x8e, rare: 0x95, ultraRare: 0x99 },
  { name: "Speed Racer", common: 0x93, rare: 0x97, ultraRare: 0x9e },
  { name: "Chim-Chim", common: 0x92, rare: 0x96, ultraRare: 0x9d },
  { name: "Taejo", common: 0x94, rare: 0x98, ultraRare: 0x9f },
  { name: "E.P. Royalton", common: 0xf9 },
  { name: "Thug", common: 0xfa },
  { name: "Ptep", common: 0xa0, rare: 0xad, ultraRare: 0xba },
  { name: "Sprocket", common: 0xa4, rare: 0xb4, ultraRare: 0xc1 },
  { name: "Vlurp", common: 0xa8, rare: 0xb5, ultraRare: 0xc2 },
  { name: "Snipe", common: 0xac, rare: 0xb9, ultraRare: 0xca },
  { name: "Dyer", common: 0xa1, rare: 0xae, ultraRare: 0xbe },
  { name: "Lucky", common: 0xa2, rare: 0xaf, ultraRare: 0xbf },
  { name: "Tank", common: 0xa3, rare: 0xb3, ultraRare: 0xc0 },
  { name: "Berger", common: 0xa9, rare: 0xb6, ultraRare: 0xc3 },
  { name: "Singe", common: 0x102, rare: 0x106, ultraRare: 0x10d },
  { name: "Raj", common: 0x103, rare: 0x107, ultraRare: 0x10e },
  { name: "Yang", common: 0x104, rare: 0x108, ultraRare: 0x10f },
  { name: "Bomble", common: 0x105, rare: 0x10c, ultraRare: 0x110 },
  { name: "Maul", common: 0x11a, rare: 0x11e, ultraRare: 0x125 },
  { name: "Nectar", common: 0x119, rare: 0x11d, ultraRare: 0x124 },
  { name: "Rastro", common: 0x117, rare: 0x11b, ultraRare: 0x122 },
  { name: "Tadd", common: 0x118, rare: 0x11c, ultraRare: 0x123 },
  { name: "Mulch", common: 0x126 },
  { name: "Ace", common: 0x127 },
];

const rarityLabels: Record<Rarity, string> = {
  common: "Common",
  rare: "Rare",
  ultraRare: "Ultra Rare",
};

function variant(row: CatalogRow, rarity: Rarity, id: number): FunkeyVariant {
  const rarityLabel = rarityLabels[rarity];
  return {
    id,
    name: row.name,
    rarity,
    label: rarity === "common" ? row.name : `${row.name} ${rarityLabel}`,
  };
}

export const funkeyFamilies: FunkeyFamily[] = catalogRows.map((row) => ({
  name: row.name,
  variants: [
    variant(row, "common", row.common),
    ...(row.rare === undefined ? [] : [variant(row, "rare", row.rare)]),
    ...(row.ultraRare === undefined ? [] : [variant(row, "ultraRare", row.ultraRare)]),
  ],
}));

export const funkeyVariants = funkeyFamilies.flatMap((family) => family.variants);

export function formatFunkeyId(id: number): string {
  return id.toString(16).toUpperCase().padStart(8, "0");
}

export function formatReport(report: Uint8Array): string {
  return Array.from(report)
    .map((byte) => byte.toString(16).toUpperCase().padStart(2, "0"))
    .join("");
}

export function reportFromId(id: number): Uint8Array {
  if (!Number.isInteger(id) || id < 0 || id > 0xffffffff) {
    throw new Error("Funkey ID must be a 32-bit unsigned integer.");
  }

  return new Uint8Array([
    0xff,
    0xff,
    0xff,
    0xf0,
    (id >>> 24) & 0xff,
    (id >>> 16) & 0xff,
    (id >>> 8) & 0xff,
    id & 0xff,
  ]);
}

export function idFromReport(report: Uint8Array): number | null {
  if (report.length !== 8) {
    return null;
  }

  if (report[0] !== 0xff || report[1] !== 0xff || report[2] !== 0xff || report[3] !== 0xf0) {
    return null;
  }

  return report[4] * 0x1000000 + report[5] * 0x10000 + report[6] * 0x100 + report[7];
}

export function findVariantById(id: number | null): FunkeyVariant | undefined {
  if (id === null) {
    return undefined;
  }

  return funkeyVariants.find((variantItem) => variantItem.id === id);
}

function normalizeName(value: string): string {
  return value.toLowerCase().replace(/[^a-z0-9]/g, "");
}

const nameTable = new Map<string, number>();

for (const row of catalogRows) {
  const baseName = normalizeName(row.name);
  nameTable.set(baseName, row.common);
  nameTable.set(`${baseName}common`, row.common);

  if (row.rare !== undefined) {
    nameTable.set(`${baseName}r`, row.rare);
    nameTable.set(`${baseName}rare`, row.rare);
  }

  if (row.ultraRare !== undefined) {
    nameTable.set(`${baseName}vr`, row.ultraRare);
    nameTable.set(`${baseName}veryrare`, row.ultraRare);
    nameTable.set(`${baseName}ur`, row.ultraRare);
    nameTable.set(`${baseName}ultrarare`, row.ultraRare);
  }
}

nameTable.set("speed", 0x93);
nameTable.set("speedracergp", 0x93);
nameTable.set("speedracerpinballgp", 0x8e);
nameTable.set("removed", 0x00000000);
nameTable.set("none", 0x00000000);

export function parseFunkeyInput(value: string): number | null {
  const trimmed = value.trim();
  if (trimmed.length === 0) {
    return null;
  }

  const nameValue = nameTable.get(normalizeName(trimmed));
  if (nameValue !== undefined) {
    return nameValue;
  }

  const hexDigits = trimmed.replace(/[^0-9a-fA-F]/g, "");
  if (hexDigits.length === 16 && hexDigits.toUpperCase().startsWith("FFFFFFF0")) {
    return Number.parseInt(hexDigits.slice(8), 16);
  }

  if (hexDigits.length >= 1 && hexDigits.length <= 8) {
    return Number.parseInt(hexDigits, 16);
  }

  return null;
}

export function rarityLabel(rarity: Rarity): string {
  return rarityLabels[rarity];
}
