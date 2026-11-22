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
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub face_confidence: Option<f64>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub face_box: Option<[i32; 4]>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub face_landmarks: Option<[[i32; 2]; 5]>,
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
                let camera = &self.camera;
                ensure!(
                    camera.face_confidence.is_some() == camera.face_box.is_some(),
                    "incomplete face metadata"
                );
                if let (Some(confidence), Some([x0, y0, x1, y1])) =
                    (camera.face_confidence, camera.face_box)
                {
                    ensure!(
                        camera.face_detected == Some(1)
                            && confidence.is_finite()
                            && (0.0..=1.0).contains(&confidence)
                            && x0 >= 0 && y0 >= 0 && x1 <= 320 && y1 <= 240
                            && x1 > x0 && y1 > y0
                            && (f64::from(x1 - x0) * f64::from(y1 - y0) - area).abs() < 0.01,
                        "invalid face metadata"
                    );
                }
                if let Some(points) = camera.face_landmarks {
                    ensure!(
                        camera.face_box.is_some()
                            && points.iter().all(|[x, y]| (0..=320).contains(x) && (0..=240).contains(y)),
                        "invalid face landmarks"
                    );
                }
            }
            SensorStatus::Error => ensure!(
                self.camera.timestamp.is_none()
                    && self.camera.face_detected.is_none()
                    && self.camera.face_area_px2.is_none()
                    && self.camera.face_confidence.is_none()
                    && self.camera.face_box.is_none()
                    && self.camera.face_landmarks.is_none(),
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

#[derive(Clone, Debug, serde::Serialize)]
pub struct QualityReport {
    pub valid_seconds: usize,
    pub max_gap_seconds: usize,
    pub stale_latest: bool,
    pub missing_seconds: usize,
    pub camera_error: usize,
    pub depth_error: usize,
    pub no_valid_depth: usize,
    pub clock_skew: usize,
    pub received_late: usize,
    pub duplicate_seconds: usize,
}

impl QualityReport {
    pub fn ready(&self) -> bool {
        self.valid_seconds >= 162 && self.max_gap_seconds <= 5 && !self.stale_latest
    }

    pub fn reason(&self) -> &'static str {
        if self.valid_seconds < 162 {
            "insufficient_valid_seconds"
        } else if self.max_gap_seconds > 5 {
            "excessive_observation_gap"
        } else if self.stale_latest {
            "stale_latest"
        } else {
            "ready"
        }
    }
}

pub fn quality_report(rows: &[Received], end: i64) -> QualityReport {
    let mut seconds = BTreeMap::new();
    let mut report = QualityReport {
        valid_seconds: 0, max_gap_seconds: 0, stale_latest: true,
        missing_seconds: 0, camera_error: 0, depth_error: 0,
        no_valid_depth: 0, clock_skew: 0, received_late: 0,
        duplicate_seconds: 0,
    };
    for row in rows {
        let m = &row.measurement;
        if m.timestamp < end - PERIOD || m.timestamp >= end {
            continue;
        }
        if seconds.contains_key(&m.timestamp) {
            report.duplicate_seconds += 1;
            continue;
        }
        if m.camera.status != SensorStatus::Ok {
            report.camera_error += 1;
        }
        if m.depth.status != SensorStatus::Ok {
            report.depth_error += 1;
        }
        if m.depth.target_status.as_ref().is_none_or(|map| !map.iter().flatten().any(|s| *s == 5)) {
            report.no_valid_depth += 1;
        }
        if m.camera.timestamp.zip(m.depth.timestamp).is_none_or(|(a, b)| a.abs_diff(b) > 1) {
            report.clock_skew += 1;
        }
        if !(-5..=5).contains(&(row.received_at - m.timestamp)) {
            report.received_late += 1;
        }
        seconds.insert(m.timestamp, m.usable(row.received_at));
    }
    let mut gap = 0usize;
    let mut last_good = None;
    for second in end - PERIOD..end {
        if seconds.get(&second) == Some(&true) {
            report.valid_seconds += 1;
            gap = 0;
            last_good = Some(second);
        } else {
            gap += 1;
            report.missing_seconds += 1;
        }
        report.max_gap_seconds = report.max_gap_seconds.max(gap);
    }
    report.stale_latest = last_good.is_none_or(|last| end - last > 5);
    report
}

pub fn prepare_window(rows: Vec<Received>, end: i64) -> Result<Window> {
    let report = quality_report(&rows, end);
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
    ensure!(report.ready(), "insufficient_data: {}", report.reason());
    Ok(Window {
        valid_seconds: seconds.len(),
        rows: seconds.into_values().collect(),
        max_gap,
    })
}
