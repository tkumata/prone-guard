use anyhow::{Result, anyhow, ensure};
use prost::Message;
use ring::{
    aead,
    rand::{SecureRandom, SystemRandom},
};
use sha2::{Digest, Sha512};
use srp::{
    ClientG3072, Group,
    groups::G3072,
    utils::{compute_m1_rfc5054, compute_m2},
};
use subtle::ConstantTimeEq;
use zeroize::Zeroizing;

// Wire tags match ESP-IDF 6.0.3 components/protocomm/proto/{session,sec2}.proto.
#[derive(Clone, Message)]
struct Session {
    #[prost(uint32, tag = "2")]
    version: u32,
    #[prost(message, optional, tag = "12")]
    security: Option<Payload>,
}

#[derive(Clone, Message)]
struct Payload {
    #[prost(uint32, tag = "1")]
    kind: u32,
    #[prost(oneof = "Body", tags = "20, 21, 22, 23")]
    body: Option<Body>,
}

#[derive(Clone, prost::Oneof)]
enum Body {
    #[prost(message, tag = "20")]
    Hello(Hello),
    #[prost(message, tag = "21")]
    Challenge(Challenge),
    #[prost(message, tag = "22")]
    Proof(Proof),
    #[prost(message, tag = "23")]
    Verified(Verified),
}

#[derive(Clone, Message)]
struct Hello {
    #[prost(bytes = "vec", tag = "1")]
    username: Vec<u8>,
    #[prost(bytes = "vec", tag = "2")]
    public: Vec<u8>,
}

#[derive(Clone, Message)]
struct Challenge {
    #[prost(uint32, tag = "1")]
    status: u32,
    #[prost(bytes = "vec", tag = "2")]
    public: Vec<u8>,
    #[prost(bytes = "vec", tag = "3")]
    salt: Vec<u8>,
}

#[derive(Clone, Message)]
struct Proof {
    #[prost(bytes = "vec", tag = "1")]
    proof: Vec<u8>,
}

#[derive(Clone, Message)]
struct Verified {
    #[prost(uint32, tag = "1")]
    status: u32,
    #[prost(bytes = "vec", tag = "2")]
    proof: Vec<u8>,
    #[prost(bytes = "vec", tag = "3")]
    nonce: Vec<u8>,
}

fn packet(kind: u32, body: Body) -> Vec<u8> {
    Session {
        version: 2,
        security: Some(Payload {
            kind,
            body: Some(body),
        }),
    }
    .encode_to_vec()
}

fn response(data: &[u8], kind: u32) -> Result<Body> {
    ensure!(data.len() <= 32763, "oversized security response");
    let session = Session::decode(data)?;
    ensure!(session.version == 2, "security version mismatch");
    let security = session
        .security
        .ok_or_else(|| anyhow!("missing security payload"))?;
    ensure!(security.kind == kind, "unexpected security message");
    security
        .body
        .ok_or_else(|| anyhow!("missing security message"))
}

pub struct Handshake {
    username: String,
    password: Zeroizing<String>,
    secret: Zeroizing<[u8; 32]>,
    public: [u8; 384],
}

pub struct Verification {
    expected: Zeroizing<Vec<u8>>,
    key: Zeroizing<Vec<u8>>,
}

pub struct Security2 {
    key: aead::LessSafeKey,
    nonce: [u8; 12],
    usable: bool,
}

impl Handshake {
    pub fn new(username: String, password: Zeroizing<String>) -> Result<Self> {
        ensure!(
            crate::pairing::valid_identity(&username),
            "invalid device identity"
        );
        ensure!(
            crate::pairing::valid_secret(&password),
            "invalid registration credential"
        );
        let mut secret = Zeroizing::new([0u8; 32]);
        SystemRandom::new()
            .fill(secret.as_mut())
            .map_err(|_| anyhow!("random source failed"))?;
        Ok(Self::with_secret(username, password, secret))
    }

    fn with_secret(
        username: String,
        password: Zeroizing<String>,
        secret: Zeroizing<[u8; 32]>,
    ) -> Self {
        let bytes = ClientG3072::<Sha512>::new().compute_public_ephemeral(secret.as_ref());
        let mut public = [0u8; 384];
        public[384 - bytes.len()..].copy_from_slice(&bytes);
        Self {
            username,
            password,
            secret,
            public,
        }
    }

    pub fn hello(&self) -> Vec<u8> {
        packet(
            0,
            Body::Hello(Hello {
                username: self.username.as_bytes().to_vec(),
                public: self.public.to_vec(),
            }),
        )
    }

    pub fn challenge(self, data: &[u8]) -> Result<(Verification, Vec<u8>)> {
        let Body::Challenge(challenge) = response(data, 1)? else {
            return Err(anyhow!("invalid challenge"));
        };
        ensure!(
            challenge.status == 0
                && challenge.salt.len() == 16
                && !challenge.public.is_empty()
                && challenge.public.len() <= 384,
            "invalid challenge"
        );
        let verifier = ClientG3072::<Sha512>::new()
            .process_reply(
                self.secret.as_ref(),
                self.username.as_bytes(),
                self.password.as_bytes(),
                &challenge.salt,
                &challenge.public,
            )
            .map_err(|_| anyhow!("invalid SRP public value"))?;
        let key = Zeroizing::new(Sha512::digest(verifier.key()).to_vec());
        // ESP-IDF hashes the transmitted 384-byte A in its proofs, including leading zeros.
        let proof = compute_m1_rfc5054::<Sha512>(
            &G3072::generator(),
            false,
            self.username.as_bytes(),
            &challenge.salt,
            &self.public,
            &challenge.public,
            &key,
        );
        let expected = Zeroizing::new(compute_m2::<Sha512>(&self.public, &proof, &key).to_vec());
        Ok((
            Verification { expected, key },
            packet(
                2,
                Body::Proof(Proof {
                    proof: proof.to_vec(),
                }),
            ),
        ))
    }
}

impl Verification {
    pub fn finish(self, data: &[u8]) -> Result<Security2> {
        let Body::Verified(verified) = response(data, 3)? else {
            return Err(anyhow!("invalid verification"));
        };
        ensure!(
            verified.status == 0
                && verified.nonce.len() == 12
                && bool::from(self.expected.ct_eq(&verified.proof)),
            "device authentication failed"
        );
        let key = aead::UnboundKey::new(&aead::AES_256_GCM, &self.key[..32])
            .map_err(|_| anyhow!("invalid session key"))?;
        Ok(Security2 {
            key: aead::LessSafeKey::new(key),
            nonce: verified.nonce.try_into().unwrap(),
            usable: true,
        })
    }
}

impl Security2 {
    fn next_nonce(&mut self) -> Result<aead::Nonce> {
        ensure!(self.usable, "security session closed");
        let counter = u32::from_be_bytes(self.nonce[8..].try_into().unwrap());
        let Some(next) = counter.checked_add(1) else {
            self.usable = false;
            return Err(anyhow!("session nonce exhausted"));
        };
        let nonce = aead::Nonce::assume_unique_for_key(self.nonce);
        self.nonce[8..].copy_from_slice(&next.to_be_bytes());
        Ok(nonce)
    }

    pub fn encrypt(&mut self, plaintext: &[u8]) -> Result<Vec<u8>> {
        ensure!(plaintext.len() <= 16384, "oversized command");
        let nonce = self.next_nonce()?;
        let mut ciphertext = plaintext.to_vec();
        if self
            .key
            .seal_in_place_append_tag(nonce, aead::Aad::empty(), &mut ciphertext)
            .is_err()
        {
            self.usable = false;
            return Err(anyhow!("command encryption failed"));
        }
        Ok(ciphertext)
    }

    pub fn decrypt(&mut self, ciphertext: &[u8]) -> Result<Zeroizing<Vec<u8>>> {
        ensure!(
            (16..=16400).contains(&ciphertext.len()),
            "invalid response size"
        );
        let nonce = self.next_nonce()?;
        let mut plaintext = Zeroizing::new(ciphertext.to_vec());
        let length = match self
            .key
            .open_in_place(nonce, aead::Aad::empty(), &mut plaintext)
        {
            Ok(value) => value.len(),
            Err(_) => {
                self.usable = false;
                return Err(anyhow!("response authentication failed"));
            }
        };
        plaintext.truncate(length);
        Ok(plaintext)
    }
}

pub fn random_hex(bytes: usize) -> Result<String> {
    let mut value = Zeroizing::new(vec![0u8; bytes]);
    SystemRandom::new()
        .fill(&mut value)
        .map_err(|_| anyhow!("random source failed"))?;
    Ok(value.iter().map(|b| format!("{b:02x}")).collect())
}

pub fn registration_verifier(id: &str, password: &str, salt: &[u8; 16]) -> Result<Vec<u8>> {
    ensure!(
        crate::pairing::valid_identity(id) && crate::pairing::valid_secret(password),
        "invalid registration credential"
    );
    Ok(ClientG3072::<Sha512>::new().compute_verifier(id.as_bytes(), password.as_bytes(), salt))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn hex(value: &str) -> Vec<u8> {
        value
            .as_bytes()
            .as_chunks::<2>()
            .0
            .iter()
            .map(|pair| u8::from_str_radix(std::str::from_utf8(pair).unwrap(), 16).unwrap())
            .collect()
    }

    #[test]
    fn matches_esp_idf_reference_transcripts_including_padded_public_values() {
        let fixture: serde_json::Value =
            serde_json::from_str(include_str!("../../tests/fixtures/security2.json")).unwrap();
        for case in fixture["cases"].as_array().unwrap() {
            let bytes = |name| hex(case[name].as_str().unwrap());
            let handshake = Handshake::with_secret(
                case["username"].as_str().unwrap().into(),
                Zeroizing::new(case["password"].as_str().unwrap().into()),
                Zeroizing::new(bytes("secret").try_into().unwrap()),
            );
            let Body::Hello(hello) = response(&handshake.hello(), 0).unwrap() else {
                panic!()
            };
            assert_eq!(hello.public, bytes("public"));
            let challenge = packet(
                1,
                Body::Challenge(Challenge {
                    status: 0,
                    public: bytes("server_public"),
                    salt: bytes("salt"),
                }),
            );
            let (verification, proof) = handshake.challenge(&challenge).unwrap();
            let Body::Proof(proof) = response(&proof, 2).unwrap() else {
                panic!()
            };
            assert_eq!(proof.proof, bytes("client_proof"));
            let verified = packet(
                3,
                Body::Verified(Verified {
                    status: 0,
                    proof: bytes("server_proof"),
                    nonce: bytes("nonce"),
                }),
            );
            let mut security = verification.finish(&verified).unwrap();
            assert_eq!(
                security
                    .encrypt(b"{\"op\":\"identity\",\"request\":0}")
                    .unwrap(),
                bytes("encrypted_command")
            );
        }
    }

    #[test]
    fn wrong_scheme_and_invalid_public_value_are_rejected() {
        let id = "1".repeat(32);
        let password = Zeroizing::new("a".repeat(64));
        let handshake = Handshake::new(id.clone(), password.clone()).unwrap();
        assert!(
            handshake
                .challenge(
                    &Session {
                        version: 0,
                        security: None
                    }
                    .encode_to_vec()
                )
                .is_err()
        );
        let handshake = Handshake::new(id, password).unwrap();
        assert!(
            handshake
                .challenge(&packet(
                    1,
                    Body::Challenge(Challenge {
                        status: 0,
                        public: vec![0; 384],
                        salt: vec![1; 16]
                    })
                ))
                .is_err()
        );
    }

    #[test]
    fn nonce_advances_and_tampering_closes_the_session() {
        fn cipher() -> Security2 {
            Security2 {
                key: aead::LessSafeKey::new(
                    aead::UnboundKey::new(&aead::AES_256_GCM, &[7; 32]).unwrap(),
                ),
                nonce: [0; 12],
                usable: true,
            }
        }
        let mut client = cipher();
        let mut device = cipher();
        let command = client.encrypt(b"command").unwrap();
        assert_eq!(&*device.decrypt(&command).unwrap(), b"command");
        let reply = device.encrypt(b"reply").unwrap();
        assert_eq!(&*client.decrypt(&reply).unwrap(), b"reply");
        assert!(client.decrypt(&reply).is_err());
        assert!(client.encrypt(b"next").is_err());
        let mut exhausted = cipher();
        exhausted.nonce[8..].copy_from_slice(&u32::MAX.to_be_bytes());
        assert!(exhausted.encrypt(b"next").is_err());
    }
}
