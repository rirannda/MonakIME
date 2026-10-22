// Copyright (C) 2026 rirannda
// SPDX-License-Identifier: GPL-3.0-or-later
//! Sorted dictionaries, read-only mmap, and bounded preference learning.
//! The large system dictionary stays on disk; lookups use binary search.
use crate::{converter::Candidate, text};
use memmap2::{Advice, Mmap, MmapOptions, UncheckedAdvice};
use std::{
    collections::BTreeSet,
    fs::{self, File, OpenOptions},
    io::{self, BufRead, BufReader, Read, Write},
    os::unix::fs::OpenOptionsExt,
    path::Path,
};

struct Entry {
    reading: String,
    surface: String,
    cost: f64,
    preferred: bool,
}
struct Learned {
    reading: String,
    surface: String,
    count: u32,
    age: u64,
}
struct Mapped {
    memory: Mmap,
    count: usize,
    blob: usize,
}

impl Mapped {
    fn u32(&self, offset: usize) -> u32 {
        u32::from_le_bytes(self.memory[offset..offset + 4].try_into().unwrap())
    }
    fn checked_field(&self, offset: usize) -> Option<&str> {
        let bytes = self.memory.get(self.blob.checked_add(offset)?..)?;
        let end = bytes.iter().take(1025).position(|&c| c == 0)?;
        let value = std::str::from_utf8(&bytes[..end]).ok()?;
        text::valid_field(value).then_some(value)
    }
    fn field(&self, offset: usize) -> &str {
        // SAFETY: load() validates the complete immutable blob as UTF-8, and every
        // referenced offset/terminator. Rechecking each binary-search probe would
        // rescan the same Japanese text thousands of times per conversion.
        let blob = unsafe { std::str::from_utf8_unchecked(&self.memory[self.blob..]) };
        blob[offset..].split_once('\0').unwrap().0
    }
    fn reading(&self, index: usize) -> &str {
        self.field(self.u32(16 + index * 12) as usize)
    }
    fn load(file: &File) -> io::Result<Self> {
        // SAFETY: this is a read-only dictionary. Writers must replace the file
        // atomically, never truncate a file while an IME has it mapped.
        let memory = unsafe { MmapOptions::new().map(file)? };
        let invalid = || io::Error::new(io::ErrorKind::InvalidData, "invalid NIMEDIC1 dictionary");
        if memory.len() < 16 {
            return Err(invalid());
        }
        let count = u32::from_le_bytes(memory[8..12].try_into().unwrap()) as usize;
        let blob_size = u32::from_le_bytes(memory[12..16].try_into().unwrap()) as usize;
        let blob = 16 + count * 12;
        if count > 2_000_000 || blob.checked_add(blob_size) != Some(memory.len()) {
            return Err(invalid());
        }
        let out = Self {
            memory,
            count,
            blob,
        };
        std::str::from_utf8(&out.memory[blob..]).map_err(|_| invalid())?;
        let mut previous = "";
        for i in 0..count {
            let record = 16 + i * 12;
            let reading = out
                .checked_field(out.u32(record) as usize)
                .ok_or_else(invalid)?;
            out.checked_field(out.u32(record + 4) as usize)
                .ok_or_else(invalid)?;
            if reading < previous || !f32::from_bits(out.u32(record + 8)).is_finite() {
                return Err(invalid());
            }
            previous = reading;
        }
        // SAFETY: only clean, file-backed pages are discarded. No mutable map exists.
        let _ = unsafe { out.memory.unchecked_advise(UncheckedAdvice::DontNeed) };
        let _ = out.memory.advise(Advice::Random);
        Ok(out)
    }
}

#[derive(Default)]
pub struct Dictionary {
    mapped: Option<Mapped>,
    entries: Vec<Entry>,
    english: BTreeSet<String>,
    learned: Vec<Learned>,
    clock: u64,
}

fn insert(out: &mut Vec<Candidate>, surface: &str, score: f64) {
    if let Some(old) = out.iter_mut().find(|c| c.text == surface) {
        old.score = old.score.max(score);
    } else {
        out.push(Candidate::new(surface, score));
    }
}

impl Dictionary {
    pub fn len(&self) -> usize {
        self.entries.len() + self.mapped.as_ref().map_or(0, |m| m.count)
    }
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }
    pub fn learning_len(&self) -> usize {
        self.learned.len()
    }
    pub fn load(&mut self, path: impl AsRef<Path>, preferred: bool) -> io::Result<()> {
        let mut file = File::open(path)?;
        let mut header = [0; 8];
        if file.read(&mut header)? == 8 && &header == b"NIMEDIC1" {
            self.mapped = Some(Mapped::load(&file)?);
            return Ok(());
        }
        use std::io::{Seek, SeekFrom};
        file.seek(SeekFrom::Start(0))?;
        let mut added = Vec::new();
        for line in BufReader::new(file).lines().take(2_000_000) {
            let line = line?;
            if line.starts_with('#') || line.len() > 4096 {
                continue;
            }
            let mut fields = line.split('\t');
            let reading = text::hiragana(fields.next().unwrap_or_default());
            let surface = fields.next().unwrap_or_default().trim_end_matches('\r');
            let cost = match fields.next() {
                Some(value) => value.trim().parse::<f64>().unwrap_or(f64::NAN),
                None => 1.0,
            };
            if text::valid_field(&reading) && text::valid_field(surface) && cost.is_finite() {
                added.push(Entry {
                    reading,
                    surface: surface.to_owned(),
                    cost: cost.clamp(0.0, 100.0),
                    preferred,
                });
            }
        }
        self.entries.extend(added);
        self.entries.sort_by(|a, b| {
            a.reading
                .cmp(&b.reading)
                .then(a.cost.total_cmp(&b.cost))
                .then(a.surface.cmp(&b.surface))
        });
        Ok(())
    }
    pub fn load_english(&mut self, path: impl AsRef<Path>) -> io::Result<()> {
        for line in BufReader::new(File::open(path)?).lines().take(200_000) {
            let word = line?.trim().to_ascii_lowercase();
            if (2..=48).contains(&word.len()) && word.bytes().all(|c| c.is_ascii_alphabetic()) {
                self.english.insert(word);
            }
        }
        Ok(())
    }
    pub fn english(&self, word: &str) -> bool {
        self.english.contains(&word.to_ascii_lowercase())
    }
    pub fn lookup(&self, reading: &str, limit: usize) -> Vec<Candidate> {
        if limit == 0 {
            return vec![];
        }
        let mut out = Vec::new();
        let start = self
            .entries
            .partition_point(|e| e.reading.as_str() < reading);
        for entry in self.entries[start..]
            .iter()
            .take_while(|e| e.reading == reading)
            .take(64)
        {
            insert(
                &mut out,
                &entry.surface,
                if entry.preferred {
                    4.0 + 1.0 / (1.0 + entry.cost)
                } else {
                    -entry.cost
                },
            );
        }
        if let Some(mapped) = &self.mapped {
            let (mut lo, mut hi) = (0, mapped.count);
            while lo < hi {
                let mid = (lo + hi) / 2;
                if mapped.reading(mid) < reading {
                    lo = mid + 1;
                } else {
                    hi = mid;
                }
            }
            for index in lo..mapped.count.min(lo + 32) {
                if mapped.reading(index) != reading {
                    break;
                }
                let record = 16 + index * 12;
                let surface = mapped.field(mapped.u32(record + 4) as usize);
                insert(
                    &mut out,
                    surface,
                    -f32::from_bits(mapped.u32(record + 8)) as f64,
                );
            }
        }
        let start = self
            .learned
            .partition_point(|e| e.reading.as_str() < reading);
        for learned in self.learned[start..]
            .iter()
            .take_while(|e| e.reading == reading)
        {
            insert(
                &mut out,
                &learned.surface,
                4.0 + (learned.count as f64).ln_1p() * 2.0,
            );
        }
        out.sort_by(|a, b| b.score.total_cmp(&a.score));
        out.truncate(limit.min(32));
        out
    }
    pub fn contains(&self, reading: &str) -> bool {
        !self.lookup(reading, 1).is_empty()
    }
    pub fn learn(&mut self, reading: &str, surface: &str) {
        if !text::valid_field(reading)
            || !text::valid_field(surface)
            || reading.chars().count() > 256
        {
            return;
        }
        self.clock += 1;
        if let Some(entry) = self
            .learned
            .iter_mut()
            .find(|e| e.reading == reading && e.surface == surface)
        {
            entry.count = (entry.count + 1).min(1_000_000);
            entry.age = self.clock;
            return;
        }
        if self.learned.len() == 4096 {
            let oldest = self
                .learned
                .iter()
                .enumerate()
                .min_by_key(|(_, e)| e.age)
                .unwrap()
                .0;
            self.learned.remove(oldest);
        }
        let pos = self
            .learned
            .partition_point(|e| e.reading.as_str() < reading);
        self.learned.insert(
            pos,
            Learned {
                reading: reading.into(),
                surface: surface.into(),
                count: 1,
                age: self.clock,
            },
        );
    }
    pub fn load_learning(&mut self, path: impl AsRef<Path>) -> io::Result<()> {
        for line in BufReader::new(File::open(path)?).lines().take(100_000) {
            let line = line?;
            if line.len() > 4096 {
                continue;
            }
            let fields: Vec<_> = line.splitn(3, '\t').collect();
            if fields.len() < 2 {
                continue;
            }
            self.learn(fields[0], fields[1]);
            if let Some(entry) = self
                .learned
                .iter_mut()
                .find(|e| e.reading == fields[0] && e.surface == fields[1])
            {
                entry.count = fields
                    .get(2)
                    .and_then(|v| v.parse().ok())
                    .unwrap_or(1)
                    .clamp(1, 1_000_000);
            }
        }
        Ok(())
    }
    pub fn save_learning(&self, path: impl AsRef<Path>) -> io::Result<()> {
        let path = path.as_ref();
        if let Some(parent) = path.parent().filter(|p| !p.as_os_str().is_empty()) {
            fs::create_dir_all(parent)?;
        }
        let mut attempt = 0;
        let (temp, mut file) = loop {
            let temp = path.with_extension(format!("tmp.{}.{}", std::process::id(), attempt));
            match OpenOptions::new()
                .write(true)
                .create_new(true)
                .mode(0o600)
                .open(&temp)
            {
                Ok(file) => break (temp, file),
                Err(e) if e.kind() == io::ErrorKind::AlreadyExists && attempt < 100 => attempt += 1,
                Err(e) => return Err(e),
            }
        };
        let result = (|| {
            for entry in &self.learned {
                writeln!(
                    file,
                    "{}\t{}\t{}",
                    entry.reading, entry.surface, entry.count
                )?;
            }
            file.sync_all()?;
            fs::rename(&temp, path)
        })();
        if result.is_err() {
            let _ = fs::remove_file(temp);
        }
        result
    }
}
