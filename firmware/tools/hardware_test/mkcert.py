"""Self-signed test CA + broker certificate (RSA 2048, SAN IP 192.168.0.138) for the local TLS broker."""
import datetime, ipaddress
from cryptography import x509
from cryptography.x509.oid import NameOID
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import rsa

now = datetime.datetime.now(datetime.timezone.utc)
def key(): return rsa.generate_private_key(public_exponent=65537, key_size=2048)
def name(cn): return x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, cn)])

ca_key = key()
ca = (x509.CertificateBuilder().subject_name(name("IRIV test CA")).issuer_name(name("IRIV test CA"))
      .public_key(ca_key.public_key()).serial_number(x509.random_serial_number())
      .not_valid_before(now - datetime.timedelta(days=1)).not_valid_after(now + datetime.timedelta(days=365))
      .add_extension(x509.BasicConstraints(ca=True, path_length=None), critical=True).sign(ca_key, hashes.SHA256()))
srv_key = key()
srv = (x509.CertificateBuilder().subject_name(name("192.168.0.138")).issuer_name(ca.subject)
       .public_key(srv_key.public_key()).serial_number(x509.random_serial_number())
       .not_valid_before(now - datetime.timedelta(days=1)).not_valid_after(now + datetime.timedelta(days=365))
       .add_extension(x509.SubjectAlternativeName([x509.IPAddress(ipaddress.ip_address("192.168.0.138")), x509.DNSName("192.168.0.138")]), critical=False)
       .sign(ca_key, hashes.SHA256()))
open("ca.pem", "wb").write(ca.public_bytes(serialization.Encoding.PEM))
open("server.pem", "wb").write(srv.public_bytes(serialization.Encoding.PEM))
open("server.key", "wb").write(srv_key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.TraditionalOpenSSL, serialization.NoEncryption()))
print("ok")
