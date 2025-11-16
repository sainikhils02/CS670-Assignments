# CS670 Assignment 1: Secure Recommendation System Updates

## Overview

This project implements a secure recommendation system using multi-party computation (MPC) with secret sharing. The system allows secure computation of user profile updates without revealing individual user preferences or item characteristics to any single party.

**Problem Statement**: Given secret-shared user profiles `U ∈ Z^(m×k)` and item profiles `V ∈ Z^(n×k)`, securely compute user profile updates:
```
u_i ← u_i + v_j(1 - ⟨u_i, v_j⟩)
```
where user `i` interacts with item `j`, without revealing the actual vectors to any party.

## Quick Start Guide

### Prerequisites
- Docker
- Compose v2 plugin (preferred) or plain Docker fallback
- Windows with WSL2 (or Linux/macOS)

### Running the System

1. **Clone and navigate to the assignment directory**:
   ```bash
   cd A1
   ```

2. **Build all components**:
    ```bash
    docker compose build
    ```

3. **Run the complete MPC protocol**:
    ```bash
    docker compose up --build
    ```

4. **View specific party logs** (optional):
    ```bash
    docker compose logs p0  # Computing party 0
    docker compose logs p1  # Computing party 1
    docker compose logs p2  # Trusted dealer
    ```

### Configuration

The system parameters can be modified in `docker-compose.yml`:
```yaml
command: ["./gen_queries", "12", "10", "2", "6"]
# Arguments: <users> <items> <features> <queries>
```

**Default Configuration**: 12 users, 10 items, 2 features, 6 queries

### Output Files

After running, check the `data/` directory for:
- `config.txt`: System dimensions
- `U0_shares.txt`, `U1_shares.txt`: User matrix shares
- `V0_shares.txt`, `V1_shares.txt`: Item matrix shares  
- `queries_p*.txt`: Query shares for each party
- `U*_shares_updated.txt`: Updated user profiles after MPC

---

## Alternative: Run without Compose (plain Docker)

If Compose is unavailable, you can run the services with plain Docker:

```bash
cd /mnt/c/Users/saini/Downloads/CS670/cs670-25-Monsoon/A1

# Clean up previous runs
docker rm -f p0 p1 p2 gen_queries 2>/dev/null || true
docker network rm mpc_net 2>/dev/null || true

# Build image
docker build -t a1-mpc:latest .

# Network for inter-container DNS
docker network create mpc_net

# Generate data (adjust args if needed)
docker run --rm --name gen_queries --network mpc_net \
    -v "$PWD/data:/app/data" a1-mpc:latest ./gen_queries 12 10 2 6

# Start trusted dealer (P2)
docker run -d --name p2 --network mpc_net \
    -v "$PWD/data:/app/data" a1-mpc:latest /app/p2

# Start parties
docker run -d --name p1 --network mpc_net \
    -v "$PWD/data:/app/data" a1-mpc:latest /app/p1
docker run -d --name p0 --network mpc_net \
    -v "$PWD/data:/app/data" a1-mpc:latest /app/p0

# (Optional) View logs
docker logs -f p2
# In another terminal
docker logs -f p1
# In another terminal
docker logs -f p0
```

After completion, updated shares are written to `data/U0_shares_updated.txt` and `data/U1_shares_updated.txt`.

---

## Troubleshooting

- Error: `KeyError: 'ContainerConfig'` when using `docker-compose`
    - Cause: Classic docker-compose v1 on newer Docker Engine versions.
    - Fix: Use Compose v2 plugin and `docker compose ...` commands.
    - Install plugin on Ubuntu/WSL:
        ```bash
        sudo apt-get update
        sudo apt-get install -y ca-certificates curl gnupg
        sudo install -m 0755 -d /etc/apt/keyrings
        curl -fsSL https://download.docker.com/linux/ubuntu/gpg | sudo gpg --dearmor -o /etc/apt/keyrings/docker.gpg
        sudo chmod a+r /etc/apt/keyrings/docker.gpg
        echo "deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/docker.gpg] https://download.docker.com/linux/ubuntu jammy stable" | sudo tee /etc/apt/sources.list.d/docker.list > /dev/null
        sudo apt-get update
        sudo apt-get install -y docker-compose-plugin
        docker compose version
        ```
    - If you have classic docker-compose installed, consider removing it:
        ```bash
        sudo apt remove -y docker-compose
        ```

---

## Verification (Direct vs MPC)

After the MPC run completes and writes updated shares:

```bash
g++ -std=gnu++20 direct_verify_simple.cpp -o direct_verify
./direct_verify
```

Expected: 0 mismatches between direct computation and MPC results (first 10 users printed).

## Technical Report

### 1. Secret Sharing Implementation

#### 1.1 Additive Secret Sharing Scheme

Our implementation uses **additive secret sharing** over a finite field `Z_{2^32}`:

- **Sharing**: To share a secret `s`, generate random `r` and compute shares `s_0 = r` and `s_1 = s - r (mod 2^32)`
- **Reconstruction**: Recover the secret as `s = s_0 + s_1 (mod 2^32)`
- **Homomorphic Properties**: 
  - Addition: `[a] + [b] = [a + b]` (computed locally)
  - Linear combinations: `c·[a] = [c·a]` for public constants

#### 1.2 Data Structures and Field Operations

```cpp
// Field element type using 64-bit integers with 2^32 modulus
using field_t = uint64_t;
constexpr field_t MODULUS = 1ULL << 32;

// Share template for any field element
template<typename T>
struct Share {
    T value;  // One party's share of the secret
    
    Share operator+(const Share& other) const {
        return Share{Field::add(value, other.value)};
    }
    
    Share operator-(const Share& other) const {
        return Share{Field::sub(value, other.value)};
    }
};
```

#### 1.3 Matrix Secret Sharing

**User Matrix U**: `m × k` matrix where each element `U[i][j]` is additively shared:
- Party P0 holds `U0[i][j]`, Party P1 holds `U1[i][j]`  
- Invariant: `U[i][j] = U0[i][j] + U1[i][j] (mod 2^32)`

**Item Matrix V**: `n × k` matrix similarly shared between P0 and P1

**Query Vectors**: Item selection vectors `e_j` (one-hot encodings) are also secret-shared to hide which items users interact with.

### 2. Secure Inner Product Computation

#### 2.1 (2+1)-Party Dot Product Protocol

The core primitive is secure computation of `⟨a, b⟩` where `a` and `b` are secret-shared vectors. We implement the protocol from the assignment specification:

**Preprocessing Phase** (by trusted dealer P2):
1. Generate random masking vectors: `X_0, X_1, Y_0, Y_1 ∈ Z_q^k`
2. Compute offset: `α = ⟨X_0^T, Y_1⟩`
3. Send to P0: `⟨X_0^T, Y_1⟩ + α` and `X_0, Y_0`
4. Send to P1: `⟨X_1^T, Y_0⟩ - α` and `X_1, Y_1`

**Online Phase**:
1. **Masking**: Each party masks their input vectors
   - P0: `D_0 = a_0 + X_0`, `t_0 = b_0 + Y_0`
   - P1: `D_1 = a_1 + X_1`, `t_1 = b_1 + Y_1`

2. **Exchange**: Parties exchange masked values `(D_0, t_0)` ↔ `(D_1, t_1)`

3. **Local Computation**: Each party computes their share:
   - P0: `⟨a, b⟩_0 = ⟨D_0, t_0 + t_1⟩ - ⟨Y_0, D_1⟩ + ⟨X_0^T, Y_1⟩ + α`
   - P1: `⟨a, b⟩_1 = ⟨D_1, t_1 + t_0⟩ - ⟨Y_1, D_0⟩ + ⟨X_1^T, Y_0⟩ - α`

#### 2.2 Implementation in Code

```cpp
awaitable<ShareField> secure_dot_product(
    tcp::socket& peer_socket,
    tcp::socket& p2_socket, 
    const ShareVectorField& a_share,
    const ShareVectorField& b_share,
    bool is_p0
) {
    // Receive preprocessing from P2
    field_t precomputed;
    co_await recv_coroutine(p2_socket, precomputed);
    
    std::vector<field_t> X(k), Y(k);
    co_await recv_vector(p2_socket, X);
    co_await recv_vector(p2_socket, Y);
    
    // Mask input vectors
    std::vector<field_t> D(k), t(k);
    for (size_t i = 0; i < k; ++i) {
        D[i] = Field::add(a_share[i].value, X[i]);
        t[i] = Field::add(b_share[i].value, Y[i]);
    }
    
    // Exchange masked values with peer
    co_await send_vector(peer_socket, D);
    co_await send_vector(peer_socket, t);
    
    std::vector<field_t> D_peer(k), t_peer(k);
    co_await recv_vector(peer_socket, D_peer);
    co_await recv_vector(peer_socket, t_peer);
    
    // Compute share using protocol formula
    field_t result = compute_protocol_share(D, t, D_peer, t_peer, Y, precomputed, is_p0);
    co_return ShareField(result);
}
```

#### 2.3 Secure Profile Update Computation

The user profile update `u_i ← u_i + v_j(1 - ⟨u_i, v_j⟩)` requires:

1. **Item Vector Computation**: `v_j = V^T e_j` using secure matrix-vector multiplication
2. **Dot Product**: `⟨u_i, v_j⟩` using the (2+1)-party protocol  
3. **Update Formula**: Compute `1 - ⟨u_i, v_j⟩` and multiply with `v_j`
4. **Profile Update**: Add result to current user profile `u_i`

All operations are performed in the secret-shared domain, maintaining privacy throughout.

### 3. Communication and Efficiency Analysis

#### 3.1 Communication Rounds

**Total Communication Rounds**: 2
- **Round 1** (Preprocessing): P2 → {P0, P1} sends masking materials
- **Round 2** (Online): P0 ↔ P1 exchange masked input vectors

#### 3.2 Communication Complexity

**Per Query Communication**:
- **Preprocessing**: P2 sends `O(k)` field elements to each party
- **Online**: Each party sends `2k` field elements (masked vectors D and t)
- **Total per query**: `O(k)` field elements

**For Q queries**: `O(Q·k)` total communication

#### 3.3 Computational Complexity

**Per Party, Per Query**:
- **Vector Operations**: `O(k)` additions for masking
- **Dot Products**: `O(k)` multiplications for local computation  
- **Profile Updates**: `O(k)` operations for user vector update

**Total**: `O(Q·k)` operations per party

#### 3.4 Network Architecture

```
┌─────────────────────────────────────────────┐
│             Docker Network: mpc_net         │
├─────────────────────────────────────────────┤
│                                             │
│  P2 (Trusted Dealer)    P0 ←→ P1           │
│       :9002              ↑    :9001         │
│         │                │      │           │
│         └────────────────┼──────┘           │
│                          │                  │
│         Preprocessing    │  Online Phase    │
│         (Masking Data)   │  (Masked Inputs) │
└─────────────────────────────────────────────┘
```

**Key Design Decisions**:
- **Asynchronous I/O**: Boost.ASIO coroutines for non-blocking network operations
- **Role-based Compilation**: Single source file compiled with different `-DROLE_p*` flags
- **Dependency Management**: Docker Compose ensures proper startup order (P2 → P1 → P0)

#### 3.5 Efficiency Considerations

**Optimizations Implemented**:
1. **Parallel Coroutines**: Multiple queries processed concurrently using `co_spawn`
2. **Field Arithmetic**: Efficient modular operations with 64-bit intermediates
3. **Memory Layout**: Contiguous vector storage for cache efficiency
4. **Network Buffering**: Vectorized send/receive operations reduce system calls

**Performance Characteristics**:
- **Scalability**: Linear in number of features `k` and queries `Q`
- **Latency**: Dominated by 2 network round-trips  
- **Throughput**: Limited by network bandwidth for large `k`

**Security vs Efficiency Trade-offs**:
- **Semi-honest Model**: Assumes honest-but-curious parties (more efficient than malicious security)
- **Information-theoretic Security**: No computational assumptions, but requires trusted dealer
- **Communication Overhead**: Factor of ~3x compared to plaintext (due to secret sharing and masking)

---

## System Architecture

### File Structure
```
A1/
├── common.hpp          # Network utilities and MPC primitives  
├── shares.hpp          # Secret sharing data structures
├── pB.cpp             # Main MPC protocol (P0/P1 roles)
├── p2.cpp             # Trusted dealer implementation
├── gen_queries.cpp    # Data generation and secret sharing
├── Dockerfile         # Build environment (GCC-12, Boost)
├── docker-compose.yml # Multi-party network orchestration
└── data/              # Generated shares and configuration
```

### Security Properties
- **Privacy**: Individual vectors remain secret throughout computation
- **Correctness**: Updates computed accurately in secret-shared domain  
- **Semi-honest Security**: Secure against honest-but-curious adversaries
- **Information-theoretic**: Security independent of computational assumptions
