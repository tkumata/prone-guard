use anyhow::{Result, ensure};
use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;

pub const PERIOD: i64 = 180;
pub const BODY_LIMIT: usize = 16 * 1024;

#[derive(Clone, Copy, Debug, Deserialize, Serialize, PartialEq)]
#[serde(rename_all = "snake_case")]
pub enum SensorStatus {
    Ok,
    Error,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct Camera {
    pub timestamp: Option<i64>,
    pub status: SensorStatus,
    pub face_detected: Option<u8>,
    pub face_area_px2: Option<f64>,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct Depth {
    pub timestamp: Option<i64>,
    pub status: SensorStatus,
    pub distance: Option<[[i16; 8]; 8]>,
    pub target_status: Option<[[u8; 8]; 8]>,
    pub unit: String,
    pub resolution: [u8; 2],
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct Measurement {
    pub boot_id: String,
    pub sequence: i64,
    pub bed_id: String,
    pub timestamp: i64,
    pub camera: Camera,
    pub depth: Depth,
}

pub fn valid_id(id: &str) -> bool {
    !id.is_empty()
        && id.len() <= 64
        && id
            .bytes()
            .all(|b| b.is_ascii_alphanumeric() || b"-_.".contains(&b))
}

impl Measurement {
    pub fn validate(&self, now: i64) -> Result<()> {
        ensure!(
            valid_id(&self.bed_id) && valid_id(&self.boot_id),
            "invalid identifier"
        );
        ensure!(self.sequence >= 0, "invalid sequence");
        ensure!(
            (1_704_067_200..=now + 5).contains(&self.timestamp),
            "invalid timestamp"
        );
        for timestamp in [self.camera.timestamp, self.depth.timestamp]
            .into_iter()
            .flatten()
        {
            ensure!(
                self.timestamp.abs_diff(timestamp) <= 1,
                "sensor timestamp mismatch"
            );
        }
        match self.camera.status {
            SensorStatus::Ok => {
                let area = self
                    .camera
                    .face_area_px2
                    .ok_or_else(|| anyhow::anyhow!("missing face area"))?;
                ensure!(
                    self.camera.timestamp.is_some() && area.is_finite(),
                    "invalid camera observation"
                );
                ensure!((0.0..=76800.0).contains(&area), "invalid face area");
                ensure!(
                    (area * 100.0 - (area * 100.0).round()).abs() < 0.00001,
                    "invalid area precision"
                );
                ensure!(
                    matches!(
                        (self.camera.face_detected, area > 0.0),
                        (Some(0), false) | (Some(1), true)
                    ),
                    "inconsistent face detection"
                );
            }
            SensorStatus::Error => ensure!(
                self.camera.timestamp.is_none()
                    && self.camera.face_detected.is_none()
                    && self.camera.face_area_px2.is_none(),
                "camera errors must use null"
            ),
        }
        ensure!(
            self.depth.resolution == [8, 8] && self.depth.unit == "mm",
            "invalid depth format"
        );
        match self.depth.status {
            SensorStatus::Ok => {
                ensure!(
                    self.depth.timestamp.is_some()
                        && self.depth.distance.is_some()
                        && self.depth.target_status.is_some(),
                    "missing depth observation"
                );
                for (distance, status) in self
                    .depth
                    .distance
                    .as_ref()
                    .unwrap()
                    .iter()
                    .flatten()
                    .zip(self.depth.target_status.as_ref().unwrap().iter().flatten())
                {
                    ensure!(
                        *status != 5 || (0..=4000).contains(distance),
                        "invalid valid-range distance"
                    );
                }
            }
            SensorStatus::Error => ensure!(
                self.depth.timestamp.is_none()
                    && self.depth.distance.is_none()
                    && self.depth.target_status.is_none(),
                "depth errors must use null"
            ),
        }
        Ok(())
    }

    fn usable(&self, received_at: i64) -> bool {
        self.camera.status == SensorStatus::Ok
            && self.depth.status == SensorStatus::Ok
            && (-5..=5).contains(&(received_at - self.timestamp))
            && self
                .depth
                .target_status
                .as_ref()
                .is_some_and(|map| map.iter().flatten().any(|s| *s == 5))
            && self
                .camera
                .timestamp
                .zip(self.depth.timestamp)
                .is_some_and(|(c, d)| (c - d).abs() <= 1)
    }
}

pub struct Received {
    pub measurement: Measurement,
    pub received_at: i64,
}

pub struct Window {
    pub rows: Vec<Measurement>,
    pub valid_seconds: usize,
    pub max_gap: usize,
}

pub fn prepare_window(rows: Vec<Received>, end: i64) -> Result<Window> {
    let mut seconds = BTreeMap::new();
    for row in rows {
        let m = row.measurement;
        if m.timestamp >= end - PERIOD && m.timestamp < end && m.usable(row.received_at) {
            seconds.entry(m.timestamp).or_insert(m);
        }
    }
    let mut gap = 0;
    let mut max_gap = 0;
    for second in end - PERIOD..end {
        gap = if seconds.contains_key(&second) {
            0
        } else {
            gap + 1
        };
        max_gap = max_gap.max(gap);
    }
    ensure!(
        seconds.len() >= 162 && max_gap <= 5,
        "insufficient_data: valid_seconds={} max_gap={max_gap}",
        seconds.len()
    );
    ensure!(
        seconds
            .last_key_value()
            .is_some_and(|(last, _)| end - last <= 5),
        "insufficient_data: stale_latest"
    );
    Ok(Window {
        valid_seconds: seconds.len(),
        rows: seconds.into_values().collect(),
        max_gap,
    })
}
