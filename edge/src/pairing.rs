use crate::security2::{Handshake, Security2};
use anyhow::{Result, anyhow, ensure};
use std::{
    io::{Read, Write},
    time::{Duration, Instant},
};
use zeroize::Zeroizing;

pub const FRAME_LIMIT: usize = 32768;

pub fn valid_identity(value: &str) -> bool {
    value.len() == 32
        && value
            .bytes()
            .all(|c| c.is_ascii_digit() || (b'a'..=b'f').contains(&c))
}

pub fn valid_secret(value: &str) -> bool {
    value.len() == 64
        && value
            .bytes()
            .all(|c| c.is_ascii_digit() || (b'a'..=b'f').contains(&c))
}

pub fn write_frame(stream: &mut impl Write, endpoint: u8, payload: &[u8]) -> Result<()> {
    ensure!(
        endpoint <= 1 && payload.len() <= FRAME_LIMIT - 5,
        "invalid USB frame"
    );
    stream.write_all(&((payload.len() + 1) as u32).to_be_bytes())?;
    stream.write_all(&[endpoint])?;
    stream.write_all(payload)?;
    stream.flush()?;
    Ok(())
}

pub fn read_frame(stream: &mut impl Read, endpoint: u8) -> Result<Vec<u8>> {
    let mut header = [0; 4];
    stream.read_exact(&mut header)?;
    let length = u32::from_be_bytes(header) as usize;
    ensure!(
        (1..=FRAME_LIMIT - 4).contains(&length),
        "invalid USB frame length"
    );
    let mut body = vec![0; length];
    stream.read_exact(&mut body)?;
    ensure!(body[0] == endpoint, "USB request rejected");
    Ok(body[1..].to_vec())
}

pub struct UsbSession {
    port: Box<dyn serialport::SerialPort>,
    security: Security2,
    deadline: Instant,
    request: u32,
    usable: bool,
}

impl UsbSession {
    pub fn connect(path: &str, id: String, password: Zeroizing<String>) -> Result<Self> {
        let mut port = serialport::new(path, 115200)
            .timeout(Duration::from_secs(1))
            .open()?;
        port.clear(serialport::ClearBuffer::All)?;
        let deadline = Instant::now() + Duration::from_secs(30);
        let hello = Handshake::new(id, password)?;
        let challenge = exchange(&mut port, 0, &hello.hello(), deadline)?;
        let (verification, proof) = hello.challenge(&challenge)?;
        let response = exchange(&mut port, 0, &proof, deadline)?;
        let security = verification.finish(&response)?;
        Ok(Self {
            port,
            security,
            deadline: Instant::now() + Duration::from_secs(180),
            request: 0,
            usable: true,
        })
    }

    pub fn command(&mut self, command: &serde_json::Value) -> Result<serde_json::Value> {
        ensure!(
            self.usable && Instant::now() < self.deadline,
            "USB session closed or expired"
        );
        self.usable = false;
        let mut value = command.clone();
        ensure!(
            value.is_object() && value.get("request").is_none(),
            "invalid command"
        );
        value["request"] = self.request.into();
        let data = Zeroizing::new(serde_json::to_vec(&value)?);
        let encrypted = self.security.encrypt(&data)?;
        let response = exchange(&mut self.port, 1, &encrypted, self.deadline)?;
        let decrypted = self.security.decrypt(&response)?;
        let value: serde_json::Value = serde_json::from_slice(&decrypted)?;
        ensure!(
            value["request"].as_u64() == Some(u64::from(self.request)),
            "USB response sequence mismatch"
        );
        self.request = self
            .request
            .checked_add(1)
            .ok_or_else(|| anyhow!("request sequence exhausted"))?;
        self.usable = true;
        ensure!(value["ok"] == true, "device rejected command");
        Ok(value)
    }
}

fn exchange(
    port: &mut Box<dyn serialport::SerialPort>,
    endpoint: u8,
    data: &[u8],
    deadline: Instant,
) -> Result<Vec<u8>> {
    let mut stream = DeadlinePort {
        port: port.as_mut(),
        deadline,
    };
    write_frame(&mut stream, endpoint, data)?;
    let response = read_frame(&mut stream, endpoint)?;
    ensure!(Instant::now() < deadline, "USB deadline exceeded");
    Ok(response)
}

struct DeadlinePort<'a> {
    port: &'a mut dyn serialport::SerialPort,
    deadline: Instant,
}

impl DeadlinePort<'_> {
    fn remaining(&mut self) -> std::io::Result<()> {
        let remaining = self
            .deadline
            .checked_duration_since(Instant::now())
            .ok_or_else(|| {
                std::io::Error::new(std::io::ErrorKind::TimedOut, "USB deadline exceeded")
            })?;
        self.port
            .set_timeout(remaining.min(Duration::from_secs(5)))?;
        Ok(())
    }
}

impl Read for DeadlinePort<'_> {
    fn read(&mut self, data: &mut [u8]) -> std::io::Result<usize> {
        self.remaining()?;
        self.port.read(data)
    }
}

impl Write for DeadlinePort<'_> {
    fn write(&mut self, data: &[u8]) -> std::io::Result<usize> {
        self.remaining()?;
        self.port.write(data)
    }
    fn flush(&mut self) -> std::io::Result<()> {
        loop {
            self.remaining()?;
            if self.port.bytes_to_write()? == 0 {
                return Ok(());
            }
            std::thread::sleep(Duration::from_millis(10));
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Cursor;

    #[test]
    fn frames_preserve_binary_data_and_reject_untrusted_lengths() {
        let mut bytes = Vec::new();
        write_frame(&mut bytes, 1, &[0, 10, 13, 255]).unwrap();
        assert_eq!(
            read_frame(&mut Cursor::new(bytes.clone()), 1).unwrap(),
            [0, 10, 13, 255]
        );
        for size in [0, 32765, u32::MAX] {
            assert!(read_frame(&mut Cursor::new(size.to_be_bytes()), 1).is_err());
        }
        assert!(read_frame(&mut Cursor::new(&bytes[..bytes.len() - 1]), 1).is_err());
        assert!(read_frame(&mut Cursor::new(bytes), 0).is_err());
        assert!(write_frame(&mut Vec::new(), 1, &vec![0; FRAME_LIMIT - 4]).is_err());
    }
}
