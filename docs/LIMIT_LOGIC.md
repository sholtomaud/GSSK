# Limit Edge Logic: IDC Treatment and Error Bounds

## 1. What a limit edge models

A `limit` edge implements Michaelis-Menten (saturation) kinetics:

```
F = k · Q_origin / (1 + Q_origin / C)
  = k · C · Q_origin / (C + Q_origin)
```

where:
- `Q_origin` is the current value of the origin (source) node
- `C` is the value of the `control_node` (the half-saturation constant)
- `k` is the maximum rate coefficient

At low Q (Q ≪ C): `F ≈ k · Q_origin` (linear, proportional to quantity)
At saturation (Q ≫ C): `F → k · C` (constant, independent of quantity)
At Q = C: `F = k · C / 2` (half-maximum rate)

This logic appears in:
- Biological uptake (nutrient-limited growth)
- Economic throughput capped by infrastructure
- Resource extraction limited by extraction capacity

---

## 2. IDC treatment via effective conductance

For IDC (Incipient Calculus) integration, the flow matrix `A` is built from
edges whose flow can be expressed as `F = g · Q_origin` for some conductance `g`.
A limit edge is nonlinear in Q, so it cannot be placed directly in `A`.

**Phase 1 solution**: linearise at the current operating point Q₀.

The effective conductance at Q₀ is:

```
g₀ = F(Q₀) / Q₀ = k · C / (C + Q₀)
```

This is then placed in the flow matrix as a standard linear entry:
`A[target][origin] += g₀`, `A[origin][origin] -= g₀`.

The IDC step then computes:

```
Q(t + dt) ≈ expm(A · dt) · Q(t) + dt · f
```

via the Padé (3,3) matrix exponential, where `A` reflects the operating-point
conductances of all limit edges.

Note: the effective conductance `g₀ = k·C/(C+Q₀)` is the exact linearisation
of `F/Q` at Q₀. As Q→0, g₀ → k (linear limit). As Q→C, g₀ → k/2 (half-max).

---

## 3. Closed-form error bound

### 3.1 Setup

Let F_exact(Q) = k·C·Q/(C+Q) (Michaelis-Menten flow)
Let F_IDC(Q) = g₀·Q = k·C·Q/(C+Q₀) (linearised flow using operating-point g₀)

During a step, Q moves from Q₀ to Q₁ = Q₀ + ΔQ. The IDC prediction uses
g₀ fixed at the start of the step.

### 3.2 Absolute error at a point Q

```
E(Q) = F_IDC(Q) − F_exact(Q)
     = k·C·Q·(1/(C+Q₀) − 1/(C+Q))
     = k·C·Q·(Q₀ − Q) / ((C+Q₀)(C+Q))
```

So |E(Q)| = k·C·Q·|ΔQ| / ((C+Q₀)(C+Q)) where ΔQ = Q − Q₀.

### 3.3 Relative error bound

Dividing by F_exact = k·C·Q/(C+Q):

```
|E(Q)| / F_exact(Q) = |Q − Q₀| / (C + Q₀)
```

In dimensionless form with ρ = Q/C, ρ₀ = Q₀/C:

```
|E| / F_exact = |ρ − ρ₀| / (1 + ρ₀)
```

**Key result**: the relative error of the effective-conductance linearisation
equals the fractional change in Q divided by (1 + ρ₀).

### 3.4 Per-step bound

The maximum change in Q over one step is bounded by the outflow:
|ΔQ| ≤ F_exact(Q₀) · dt = k·C·ρ₀·dt / (1+ρ₀)

So the per-step relative error bound is:

```
|E| / F_exact ≤ [k·C·ρ₀·dt / (1+ρ₀)] / (C + Q₀)
              = k·ρ₀·dt / (1+ρ₀)²
              = k·(Q₀/C)·dt / (1 + Q₀/C)²
```

In practical notation with Q₀ and C explicit:

```
relative_error_bound = k · Q₀ · C · dt / (C + Q₀)²
```

### 3.5 Interpretation and practical guidance

| Q₀/C ratio | Relative error bound | Notes |
|---|---|---|
| Q₀ ≪ C (dilute) | ≈ k·Q₀·dt/C ≈ 0 | Linear regime — Padé is near-exact |
| Q₀ = C (half-sat) | = k·C·dt / 4 | Moderate — keep k·dt ≪ 4 |
| Q₀ ≫ C (saturated) | ≈ k·C·dt/Q₀ ≈ 0 | Saturated — flow barely changes with Q |

**The error is largest near Q₀ = C** (the half-saturation point), where the
nonlinearity is steepest. It is small in both the linear (Q₀ ≪ C) and
fully-saturated (Q₀ ≫ C) regimes.

**Rule of thumb**: for confidence ≤ 1% relative error, keep:
```
k · min(Q₀, C) · dt / (C + Q₀)² ≤ 0.01
```
This can always be satisfied by reducing `dt`.

### 3.6 Why this is still better than full RK4 fallback

In the pre-Phase-1 kernel, limit edges caused the IDC solver to be disabled
entirely (silent fallback to RK4). This meant:
- No IDC/RK4 cross-validation was performed
- No per-edge error estimate was reported
- The user had no quantitative information about solver quality

With Phase 1, the effective-conductance linearisation:
1. Keeps IDC running — enabling cross-validation
2. Produces a per-edge error estimate (GSSK_GetEdgeErrorEstimate) that reflects
   the actual IDC vs RK4 flow disagreement
3. Falls back to the RK4 result automatically if step_error > solver_tolerance
4. Gives the user a measurable signal to adjust dt if needed

---

## 4. The Padé approximation error (separate from linearisation)

The total IDC error for a limit edge has two components:

1. **Linearisation error** (§3): from approximating g(Q) ≈ g₀ for all Q in [Q₀, Q₁]
2. **Matrix exponential error**: from the Padé (3,3) approximation of expm(A·dt)

The Padé (3,3) global truncation error for expm(X) is:

```
|expm(X) − R₃₃(X)| ≤ c · ‖X‖⁷  (for ‖X‖ reasonably small)
```

For our case X = A·dt with dominant eigenvalue −g₀:
```
Padé error ≈ c · (g₀ · dt)⁷
```

The linearisation error scales as (g₀·dt), while the Padé error scales as
(g₀·dt)⁷. For practical simulation (g₀·dt ≪ 1), the Padé error is negligible
compared to the linearisation error, and the dominant error is from §3.

For large g₀·dt (coarse stepping), both errors grow, but the Padé error grows
much faster. This signals that dt should be reduced.

---

## 5. Alternative: quasi-steady-state reformulation

For users who need exactness at the cost of model complexity, a limit edge
can be decomposed into two linear stages:

```
S (intermediate storage) with initial_value = 0
Edge 1: origin → S, logic: linear, k = k_on × C  (binding)
Edge 2: S → origin, logic: linear, k = k_off       (unbinding)
Edge 3: S → target, logic: linear, k = k_cat       (catalysis)
```

At quasi-steady state, [S] = k_on·C·[origin] / (k_off + k_cat), giving:
F = k_cat·[S] = k_on·k_cat·C·[origin] / (k_off + k_cat)

Setting k = k_on·k_cat·C / (k_off + k_cat) recovers the Michaelis-Menten
formula with Km = (k_off + k_cat) / k_on.

This decomposition is fully IDC-eligible (all linear edges), incurs no
linearisation error, and uses the exact Padé matrix exponential. The trade-off
is two extra nodes and two extra edges per original limit edge.

This approach is recommended when:
- Q/C ≈ 1 (maximum linearisation error region)
- The system spends many steps near the half-saturation point
- Per-step error estimates consistently exceed tolerance despite small dt
