import fs from "fs";
import admin from "firebase-admin";
import { SerialPort } from "serialport";
import { ReadlineParser } from "@serialport/parser-readline";

// ---- Firebase Admin setup ----
const serviceAccount = JSON.parse(
  fs.readFileSync("./serviceAccountKey.json", "utf8")
);

console.log("Service account project:", serviceAccount.project_id);

admin.initializeApp({
  credential: admin.credential.cert(serviceAccount),
});

const db = admin.firestore();

// ---- Serial settings ----
const port = new SerialPort({ path: "COM3", baudRate: 115200 });
const parser = port.pipe(new ReadlineParser({ delimiter: "\n" }));

const deviceId = "logger-01";

console.log("Listening on COM3 @115200... (close Arduino Serial Monitor)");

// ---- Graceful shutdown on Ctrl+C ----
process.on("SIGINT", async () => {
  console.log("\nStopping uploader...");
  try {
    if (port.isOpen) {
      await new Promise((resolve) => port.close(resolve));
      console.log("Serial port closed.");
    }
  } catch (err) {
    console.error("Error closing port:", err.message);
  }
  process.exit(0);
});

parser.on("data", async (line) => {
  line = line.trim();
  if (!line) return;

  let data;
  try {
    data = JSON.parse(line);
  } catch {
    console.log("Bad JSON:", line);
    return;
  }

  const hasReading =
    typeof data.temp === "number" &&
    typeof data.humidity === "number" &&
    typeof data.pressure === "number" &&
    (typeof data.soil === "number" || typeof data.soil === "string");

  if (!hasReading) {
    console.log("Skipping:", data);
    return;
  }

  const soil = Number(data.soil);

  if (data.temp < -20 || data.temp > 85) return;
  if (data.humidity < 0 || data.humidity > 100) return;
  if (data.pressure < 300 || data.pressure > 1100) return;
  if (!Number.isFinite(soil)) return;

  const doc = {
    deviceId,
    temp: data.temp,
    humidity: data.humidity,
    pressure: data.pressure,
    soil: Math.round(soil),
    ts: admin.firestore.FieldValue.serverTimestamp(),
    tsClient: new Date().toISOString(),
  };

  try {
    await db.collection("final").add(doc);
    console.log("Uploaded:", doc);
  } catch (e) {
    console.error("Firestore upload failed:", e.message);
  }
});