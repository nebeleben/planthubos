import { createReadStream, createWriteStream, mkdirSync } from 'node:fs'
import { createGzip } from 'node:zlib'
import { pipeline } from 'node:stream/promises'

const FILES = ['index.html', 'app.js', 'app.css']
mkdirSync('dist-gz', { recursive: true })
for (const f of FILES) {
  await pipeline(
    createReadStream(`dist/${f}`),
    createGzip({ level: 9 }),
    createWriteStream(`dist-gz/${f}.gz`),
  )
  console.log(`dist-gz/${f}.gz`)
}
