//! Lenient ("flexible") argument deserializers.
//!
//! Tool arguments arrive as free-form JSON emitted by language models and
//! relayed through provider streams. Models routinely emit *almost* correct
//! types: `"200"` instead of `200`, `"true"` instead of `true` — especially
//! for fields whose schema is a union like `["integer", "null"]`, where
//! emitters tend to get sloppy.
//!
//! Firmius therefore treats argument JSON as untrusted input: these
//! deserializers accept the correct type *and* the common stringified
//! mistakes, coercing them instead of failing the whole tool call. Genuine
//! garbage (`"abc"` for a number) still yields a clear
//! `ToolError::InvalidArguments`.

use serde::de::{Error, Visitor};
use serde::Deserializer;
use std::fmt;

macro_rules! flex_int_visitor {
    ($t:ty, $name:ident) => {
        struct $name;
        impl<'de> Visitor<'de> for $name {
            type Value = $t;

            fn expecting(&self, f: &mut fmt::Formatter) -> fmt::Result {
                write!(
                    f,
                    "an integer, or a string containing one, fitting {}",
                    stringify!($t)
                )
            }

            fn visit_u64<E: Error>(self, v: u64) -> Result<Self::Value, E> {
                <$t>::try_from(v)
                    .map_err(|_| E::custom(format!("{v} is out of range for {}", stringify!($t))))
            }

            fn visit_i64<E: Error>(self, v: i64) -> Result<Self::Value, E> {
                <$t>::try_from(v)
                    .map_err(|_| E::custom(format!("{v} is out of range for {}", stringify!($t))))
            }

            fn visit_f64<E: Error>(self, v: f64) -> Result<Self::Value, E> {
                // Tolerate `200.0`; reject fractional or out-of-range floats.
                if v.fract() == 0.0 && v >= 0.0 && v <= <$t>::MAX as f64 {
                    Ok(v as $t)
                } else {
                    Err(E::custom(format!(
                        "{v} is not an integer fitting {}",
                        stringify!($t)
                    )))
                }
            }

            fn visit_str<E: Error>(self, v: &str) -> Result<Self::Value, E> {
                v.trim()
                    .parse::<$t>()
                    .map_err(|_| E::custom(format!("'{v}' is not an integer")))
            }
        }
    };
}

flex_int_visitor!(usize, FlexUsize);
flex_int_visitor!(u64, FlexU64);
flex_int_visitor!(u16, FlexU16);

/// Deserialize `Some(T)` or `None`/null, delegating the inner value to
/// `inner` via `deserialize_any` (fine: args always arrive as JSON).
fn opt<'de, D, T, V>(d: D, inner: V) -> Result<Option<T>, D::Error>
where
    D: Deserializer<'de>,
    V: Visitor<'de, Value = T>,
{
    struct OptVisitor<V>(V);

    impl<'de, T, V> Visitor<'de> for OptVisitor<V>
    where
        V: Visitor<'de, Value = T>,
    {
        type Value = Option<T>;

        fn expecting(&self, f: &mut fmt::Formatter) -> fmt::Result {
            write!(f, "null, or ")?;
            self.0.expecting(f)
        }

        fn visit_none<E: Error>(self) -> Result<Self::Value, E> {
            Ok(None)
        }

        fn visit_unit<E: Error>(self) -> Result<Self::Value, E> {
            Ok(None)
        }

        fn visit_some<D2: Deserializer<'de>>(self, d: D2) -> Result<Self::Value, D2::Error> {
            d.deserialize_any(self.0).map(Some)
        }
    }

    d.deserialize_option(OptVisitor(inner))
}

/// Integer accepting `123`, `"123"`, `" 123 "`, or `123.0`.
pub fn usize_<'de, D: Deserializer<'de>>(d: D) -> Result<usize, D::Error> {
    d.deserialize_any(FlexUsize)
}

/// Like [`usize_`], but also accepts null.
pub fn usize_opt<'de, D: Deserializer<'de>>(d: D) -> Result<Option<usize>, D::Error> {
    opt(d, FlexUsize)
}

/// Like [`usize_opt`] for `u64` (e.g. millisecond timeouts).
pub fn u64_opt<'de, D: Deserializer<'de>>(d: D) -> Result<Option<u64>, D::Error> {
    opt(d, FlexU64)
}

/// Like [`usize_opt`] for `u16` (e.g. terminal rows/columns).
pub fn u16_opt<'de, D: Deserializer<'de>>(d: D) -> Result<Option<u16>, D::Error> {
    opt(d, FlexU16)
}

struct FlexBool;
impl<'de> Visitor<'de> for FlexBool {
    type Value = bool;

    fn expecting(&self, f: &mut fmt::Formatter) -> fmt::Result {
        f.write_str("a boolean, or the string \"true\"/\"false\"")
    }

    fn visit_bool<E: Error>(self, v: bool) -> Result<Self::Value, E> {
        Ok(v)
    }

    fn visit_str<E: Error>(self, v: &str) -> Result<Self::Value, E> {
        match v.trim().to_ascii_lowercase().as_str() {
            "true" => Ok(true),
            "false" => Ok(false),
            other => Err(E::custom(format!("'{other}' is not a boolean"))),
        }
    }
}

/// Boolean accepting `true`, `"true"`, `"FALSE"`, …
pub fn bool_<'de, D: Deserializer<'de>>(d: D) -> Result<bool, D::Error> {
    d.deserialize_any(FlexBool)
}