// Writes a fake ESP-IDF application image for testing the OTA page against
// the PC simulator (header + app descriptor only, padded with 0xFF).
//   node scripts/fake-ota.mjs out.bin [version] [project] [chipId]
import { writeFileSync } from 'node:fs';

const [out = 'fake-grainmeter.bin', version = '9.9.9-test', project = 'grainmeter', chip = '13'] =
  process.argv.slice(2);
const size = 8192;
const b = Buffer.alloc(size, 0xff);
b.fill(0, 0, 256);
b[0] = 0xe9; // ESP image magic
b[1] = 1; // segment count
b.writeUInt16LE(Number(chip), 12); // chip id: 0x000D = ESP32-C6
b.writeUInt32LE(0xabcd5432, 32); // esp_app_desc_t magic
b.write(version.slice(0, 31), 48, 'latin1');
b.write(project.slice(0, 31), 80, 'latin1');
writeFileSync(out, b);
console.log(`${out}: ${size} B, version ${version}, project ${project}, chip ${chip}`);
