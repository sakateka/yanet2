use std::net::Ipv6Addr;

/// Parse IPv6 address from string to bytes
pub fn parse_ipv6(s: &str) -> Result<Vec<u8>, Box<dyn std::error::Error>> {
    let addr: Ipv6Addr = s.parse()?;
    Ok(addr.octets().to_vec())
}

/// Format IPv6 address from bytes to string
pub fn format_ipv6(bytes: &[u8]) -> String {
    if bytes.len() != 16 {
        return format!("invalid IPv6 ({} bytes)", bytes.len());
    }
    let mut octets = [0u8; 16];
    octets.copy_from_slice(bytes);
    Ipv6Addr::from(octets).to_string()
}

/// Parse MAC address from string to bytes
pub fn parse_mac(s: &str) -> Result<Vec<u8>, Box<dyn std::error::Error>> {
    let parts: Vec<&str> = s.split(':').collect();
    if parts.len() != 6 {
        return Err(format!("invalid MAC address format: {}", s).into());
    }

    let mut bytes = Vec::with_capacity(6);
    for part in parts {
        let byte = u8::from_str_radix(part, 16).map_err(|_| format!("invalid MAC address byte: {}", part))?;
        bytes.push(byte);
    }

    Ok(bytes)
}

/// Format MAC address from bytes to string
pub fn format_mac(bytes: &[u8]) -> String {
    if bytes.len() != 6 {
        return format!("invalid MAC ({} bytes)", bytes.len());
    }
    format!(
        "{:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}",
        bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5]
    )
}