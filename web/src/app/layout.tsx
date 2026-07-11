import type { Metadata } from "next";
import "./globals.css";

export const metadata: Metadata = {
  title: "Funkey Shifter",
  description: "Web Bluetooth controller for the FunkeyShifter ESP32 portal",
};

export default function RootLayout({
  children,
}: Readonly<{
  children: React.ReactNode;
}>) {
  return (
    <html lang="en">
      <body suppressHydrationWarning>{children}</body>
    </html>
  );
}
