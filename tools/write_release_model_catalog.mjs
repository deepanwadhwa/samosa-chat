import fs from 'node:fs';

const catalog = JSON.parse(fs.readFileSync(process.argv[2], 'utf8'));
catalog.models = catalog.models.filter(model => model.family !== 'chat' || model.id === 'qwen');
if (catalog.models.filter(model => model.family === 'chat').length !== 1) {
  throw new Error('The release catalog must contain exactly one Qwen chat entry.');
}
process.stdout.write(JSON.stringify(catalog, null, 2) + '\n');
