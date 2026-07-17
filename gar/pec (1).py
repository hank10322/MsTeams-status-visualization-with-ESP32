"""Automatic sparse Pauli-Lindblad probabilistic error cancellation.

The public interface is intentionally small::

    myPEC = PEC(
        noise_circuit,
        repeat=10,
        seed=412220419,
        shots=2048,
        observable="XXXXX",
    )

    result = myPEC.run()
    print(myPEC.overhead())

Model assumptions
-----------------
1. ``noise_circuit`` represents one noisy implementation of a Clifford layer
   ``U_tilde = Lambda o U``.
2. For Aer simulation, explicit non-unitary channel instructions must appear
   after the ideal unitary part of the circuit.  Alternatively, set a noisy
   backend with ``set_backend``.
3. The learned channel is a sparse Pauli-Lindblad model containing all
   one-qubit Pauli generators and two-qubit Pauli generators on neighboring
   two-qubit gate edges found in the circuit.
4. Noise is treated as layer-local, Markovian and Pauli diagonal.  Sampled
   Pauli corrections are assumed to be ideal/virtual.
5. Automatic Pauli propagation requires the ideal layer to be Clifford.

The class learns fidelity-pair decay with repeated noisy ``U U^dagger`` echo
cycles, adds rank-completing direct-fidelity measurements when needed, solves
for non-negative Lindblad rates with NNLS, constructs the inverse QPD, samples
QPD correction sets, and estimates a requested Pauli expectation value.
"""

from __future__ import annotations

from collections import Counter, defaultdict
from copy import deepcopy
import math
import warnings
from typing import Any, Dict, Iterable, List, Optional, Sequence, Tuple

import numpy as np
import pandas as pd
from scipy.optimize import nnls

from qiskit import QuantumCircuit
from qiskit.quantum_info import Clifford, Operator, Pauli
from qiskit_aer import AerSimulator


class PEC:
    """Automatic PEC for a noisy Clifford circuit layer.

    Parameters
    ----------
    noise_circuit:
        One noisy circuit layer.  For explicit Aer noise, the circuit should
        contain an ideal Clifford unitary followed by trailing channel
        instructions.  Do not include measurement operations.
    repeat:
        Maximum echo depth used to fit the Pauli-fidelity decay.  The class
        uses depths ``0, 1, ..., repeat``.  This parameter does not repeat the
        final target circuit; it controls noise-learning depth.
    seed:
        Seed used for benchmark shot sampling and QPD sampling.
    shots:
        Benchmark shots per circuit and total PEC Monte-Carlo sample budget.
        During PEC, sampled duplicate QPD sets are aggregated and run with the
        corresponding multiplicity.
    observable:
        Pauli label to measure after PEC.  Its length must equal the number of
        qubits, using Qiskit's order ``q[n-1] ... q[0]``.

    Other Parameters
    ----------------
    backend:
        Aer-compatible backend.  Defaults to ``AerSimulator()``.  A noisy
        backend can be installed later with :meth:`set_backend`.
    cutoff:
        Learned rates at or below this value are omitted from QPD sampling.
    nearest_only:
        If True, only two-qubit gate edges ``|q_i-q_j| == 1`` are included.
    store_circuits:
        If True, sampled unique QPD circuits remain available through
        :meth:`qpd_circuits`.
    """

    _NONUNITARY_NAMES = {
        "measure",
        "reset",
        "initialize",
        "kraus",
        "superop",
        "quantum_channel",
        "pauli_lindblad_error",
        "roerror",
    }

    def __init__(
        self,
        noise_circuit: QuantumCircuit,
        repeat: int = 10,
        seed: Optional[int] = None,
        shots: int = 2048,
        observable: Optional[str] = None,
        *,
        backend: Optional[Any] = None,
        cutoff: float = 1e-8,
        nearest_only: bool = True,
        store_circuits: bool = True,
    ) -> None:
        if not isinstance(noise_circuit, QuantumCircuit):
            raise TypeError("noise_circuit must be a qiskit.QuantumCircuit.")
        if int(repeat) < 2:
            raise ValueError("repeat must be at least 2 for decay fitting.")
        if int(shots) < 2:
            raise ValueError("shots must be at least 2.")
        if float(cutoff) < 0:
            raise ValueError("cutoff must be non-negative.")

        self.noise_circuit = noise_circuit.copy()
        self.repeat = int(repeat)
        self.seed = None if seed is None else int(seed)
        self.shots = int(shots)
        self.cutoff = float(cutoff)
        self.nearest_only = bool(nearest_only)
        self.store_circuits = bool(store_circuits)
        self.backend = backend if backend is not None else AerSimulator()

        self.n_qubits = self.noise_circuit.num_qubits
        self.observable = self._validate_pauli_label(observable, "observable")

        self._layer_circuit, self._ideal_layer, self._noise_suffix = (
            self._split_noisy_layer(self.noise_circuit)
        )
        self._validate_clifford_layer()

        self._edges = self._infer_neighbor_edges()
        self._model_terms = self._build_noise_basis()
        self._benchmark_paulis = list(self._model_terms)

        self._M = self._build_direct_matrix()
        self._M_pair, self._partner_paulis = self._build_pair_matrix()

        self._fit_complete = False
        self._qpd_complete = False
        self._sample_complete = False
        self._run_complete = False

        self._pair_results: List[Dict[str, Any]] = []
        self._direct_results: List[Dict[str, Any]] = []
        self._selected_direct_paulis: List[str] = []
        self._M_fit: Optional[np.ndarray] = None
        self._eta_fit: Optional[np.ndarray] = None
        self._learned_lambdas: Optional[np.ndarray] = None
        self._nnls_residual: Optional[float] = None

        self._qpd_terms: List[Dict[str, Any]] = []
        self._gamma: Optional[float] = None

        self._qpd_records: List[Dict[str, Any]] = []
        self._qpd_circuit_records: List[Dict[str, Any]] = []
        self._result: Optional[Dict[str, Any]] = None

        if len(self._noise_suffix.data) == 0 and backend is None:
            warnings.warn(
                "No explicit trailing noise channel was detected and the "
                "default AerSimulator is ideal. The learned rates will be "
                "approximately zero unless you call set_backend(noisy_backend).",
                RuntimeWarning,
                stacklevel=2,
            )

    # ------------------------------------------------------------------
    # Public workflow
    # ------------------------------------------------------------------
    def fit(self, force: bool = False) -> pd.DataFrame:
        """Learn Pauli-Lindblad rates using echo decay, direct completion and NNLS."""
        if self._fit_complete and not force:
            return self.noise_model()

        self._reset_after_fit()

        pair_eta = self._run_pair_benchmarks()
        selected, M_augmented = self._select_rank_completing_direct_paulis()
        self._selected_direct_paulis = selected

        direct_eta = self._run_direct_benchmarks(selected)
        direct_indices = [self._benchmark_paulis.index(p) for p in selected]
        M_direct = self._M[direct_indices, :] if direct_indices else np.empty(
            (0, self._M.shape[1]), dtype=float
        )

        self._M_fit = np.vstack([self._M_pair, M_direct])
        self._eta_fit = np.concatenate([pair_eta, direct_eta])

        rank = int(np.linalg.matrix_rank(self._M_fit))
        if rank != len(self._model_terms):
            raise RuntimeError(
                "The fitted matrix is not full rank: "
                f"rank={rank}, unknowns={len(self._model_terms)}."
            )

        learned, residual = nnls(self._M_fit, self._eta_fit)
        self._learned_lambdas = np.asarray(learned, dtype=float)
        self._nnls_residual = float(residual)
        self._fit_complete = True

        self._build_qpd()
        return self.noise_model()

    def run(self, force: bool = False) -> Dict[str, Any]:
        """Run the complete workflow and return the PEC estimate."""
        if self._run_complete and not force:
            return dict(self._result or {})

        self._ensure_qpd()
        self.sample_qpd_sets(force=force)
        self._execute_sampled_qpd_circuits()
        return dict(self._result or {})

    def run_all(self, force: bool = False) -> Dict[str, Any]:
        """Alias of :meth:`run`."""
        return self.run(force=force)

    def set_backend(self, backend: Any) -> "PEC":
        """Replace the execution backend and clear all learned/run results."""
        if backend is None or not hasattr(backend, "run"):
            raise TypeError("backend must provide a run(circuits, ...) method.")
        self.backend = backend
        self._reset_all_results()
        return self

    # ------------------------------------------------------------------
    # Requested feedback methods
    # ------------------------------------------------------------------
    def overhead(self, details: bool = False) -> Any:
        """Return the QPD normalization gamma (and gamma^2 when requested)."""
        self._ensure_qpd()
        gamma = float(self._gamma)
        if details:
            return {
                "gamma": gamma,
                "sampling_cost_factor": gamma**2,
                "active_qpd_terms": len(self._qpd_terms),
            }
        return gamma

    def noise_basis(self) -> List[str]:
        """Return the automatically generated one- and two-qubit Pauli basis."""
        return list(self._model_terms)

    def neighbor_edges(self) -> List[Tuple[int, int]]:
        """Return the inferred two-qubit gate edges."""
        return list(self._edges)

    def noise_model(self) -> pd.DataFrame:
        """Return the NNLS-learned noise model and QPD probabilities."""
        self._ensure_qpd()
        qpd_by_label = {term["pauli"]: term for term in self._qpd_terms}
        rows = []
        for label, rate in zip(self._model_terms, self._learned_lambdas):
            term = qpd_by_label.get(label)
            rows.append(
                {
                    "Pauli term": label,
                    "lambda": float(rate),
                    "active": bool(term is not None),
                    "p_identity": 1.0 if term is None else term["p_identity"],
                    "p_pauli": 0.0 if term is None else term["p_pauli"],
                }
            )
        return pd.DataFrame(rows)

    def qpd_terms(self) -> pd.DataFrame:
        """Return the inverse-QPD term table."""
        self._ensure_qpd()
        return pd.DataFrame(deepcopy(self._qpd_terms))

    def sample_qpd_sets(self, force: bool = False) -> pd.DataFrame:
        """Sample ``shots`` QPD realizations and aggregate identical sets."""
        if self._sample_complete and not force:
            return self.qpd_sets()

        self._ensure_qpd()
        self._qpd_records = []
        self._qpd_circuit_records = []
        self._run_complete = False
        self._result = None

        rng = np.random.default_rng(self.seed)
        sampled_sets: List[Tuple[str, ...]] = []

        for _ in range(self.shots):
            selected = tuple(
                term["pauli"]
                for term in self._qpd_terms
                if rng.random() < term["p_pauli"]
            )
            sampled_sets.append(selected)

        counts = Counter(sampled_sets)
        for selected, count in counts.items():
            sign = -1 if len(selected) % 2 else 1
            theoretical_probability = self._qpd_set_probability(selected)
            record = {
                "QPD set": "I" if not selected else " * ".join(selected),
                "selected_terms": selected,
                "num_corrections": len(selected),
                "count": int(count),
                "empirical_probability": float(count / self.shots),
                "theoretical_probability": float(theoretical_probability),
                "sign": int(sign),
                "weight": float(sign * self._gamma),
            }
            self._qpd_records.append(record)

        self._qpd_records.sort(key=lambda row: row["count"], reverse=True)

        if self.store_circuits:
            self._qpd_circuit_records = [
                {
                    **record,
                    "circuit": self._make_pec_circuit(record["selected_terms"]),
                }
                for record in self._qpd_records
            ]

        self._sample_complete = True
        return self.qpd_sets()

    def qpd_sets(self) -> pd.DataFrame:
        """Return unique sampled QPD sets, probabilities, signs and weights."""
        if not self._sample_complete:
            self.sample_qpd_sets()
        rows = []
        for record in self._qpd_records:
            rows.append({k: v for k, v in record.items() if k != "selected_terms"})
        return pd.DataFrame(rows)

    def sampled_noise(self) -> pd.DataFrame:
        """Alias of :meth:`qpd_sets`."""
        return self.qpd_sets()

    def qpd_circuits(self) -> List[QuantumCircuit]:
        """Return unique sampled QPD circuits.

        The circuit multiplicity is available in :meth:`qpd_sets` under
        ``count``.  ``store_circuits=True`` is required.
        """
        if not self.store_circuits:
            raise RuntimeError(
                "Circuits were not stored. Recreate PEC with store_circuits=True."
            )
        if not self._sample_complete:
            self.sample_qpd_sets()
        return [record["circuit"] for record in self._qpd_circuit_records]

    def expectation(self) -> float:
        """Return the mitigated expectation value, running PEC lazily if needed."""
        if not self._run_complete:
            self.run()
        return float(self._result["pec_expectation"])

    def variance(self) -> float:
        """Return the sample variance of the weighted single-shot estimator."""
        if not self._run_complete:
            self.run()
        return float(self._result["variance"])

    def standard_error(self) -> float:
        """Return the standard error of the PEC estimator."""
        if not self._run_complete:
            self.run()
        return float(self._result["standard_error"])

    def result(self) -> Dict[str, Any]:
        """Return the complete PEC result dictionary."""
        if not self._run_complete:
            self.run()
        return dict(self._result or {})

    def summary(self) -> Dict[str, Any]:
        """Return a compact summary of model learning and PEC execution."""
        if not self._run_complete:
            self.run()
        return {
            "num_qubits": self.n_qubits,
            "neighbor_edges": self.neighbor_edges(),
            "number_of_noise_terms": len(self._model_terms),
            "pair_matrix_rank": int(np.linalg.matrix_rank(self._M_pair)),
            "fit_matrix_rank": int(np.linalg.matrix_rank(self._M_fit)),
            "selected_direct_paulis": list(self._selected_direct_paulis),
            "nnls_residual": float(self._nnls_residual),
            "overhead": float(self._gamma),
            "sampling_cost_factor": float(self._gamma**2),
            "observable": self.observable,
            "shots": self.shots,
            "pec_expectation": float(self._result["pec_expectation"]),
            "variance": float(self._result["variance"]),
            "standard_error": float(self._result["standard_error"]),
        }

    def pair_decay_results(self) -> pd.DataFrame:
        """Return all fitted echo-decay data."""
        if not self._fit_complete:
            self.fit()
        rows = []
        for record in self._pair_results:
            rows.append(
                {
                    "Pauli": record["pauli"],
                    "Partner": record["partner"],
                    "fidelity_pair": record["fidelity_pair"],
                    "eta_pair": record["eta_pair"],
                    "slope": record["slope"],
                    "intercept": record["intercept"],
                    "r_squared": record["r_squared"],
                }
            )
        return pd.DataFrame(rows)

    def direct_fidelity_results(self) -> pd.DataFrame:
        """Return rank-completing direct-fidelity measurements."""
        if not self._fit_complete:
            self.fit()
        return pd.DataFrame(deepcopy(self._direct_results))

    # ------------------------------------------------------------------
    # Circuit/model construction
    # ------------------------------------------------------------------
    def _validate_pauli_label(self, label: Optional[str], name: str) -> str:
        if label is None:
            raise ValueError(f"{name} must be provided.")
        label = str(label).upper()
        if len(label) != self.n_qubits:
            raise ValueError(
                f"{name} length must be {self.n_qubits}, got {len(label)}."
            )
        if any(char not in "IXYZ" for char in label):
            raise ValueError(f"{name} may contain only I, X, Y and Z.")
        if set(label) == {"I"}:
            raise ValueError("The all-identity observable is not informative.")
        return label

    @staticmethod
    def _instruction_parts(item: Any) -> Tuple[Any, Sequence[Any], Sequence[Any]]:
        if hasattr(item, "operation"):
            return item.operation, item.qubits, item.clbits
        return item[0], item[1], item[2]

    @staticmethod
    def _operation_is_unitary(operation: Any) -> bool:
        if getattr(operation, "name", "") in PEC._NONUNITARY_NAMES:
            return False
        if getattr(operation, "num_clbits", 0):
            return False
        try:
            return bool(Operator(operation).is_unitary())
        except Exception:
            return False

    def _split_noisy_layer(
        self, circuit: QuantumCircuit
    ) -> Tuple[QuantumCircuit, QuantumCircuit, QuantumCircuit]:
        layer = QuantumCircuit(self.n_qubits)
        ideal = QuantumCircuit(self.n_qubits)
        noise = QuantumCircuit(self.n_qubits)
        seen_noise = False

        for item in circuit.data:
            operation, qargs, cargs = self._instruction_parts(item)
            name = getattr(operation, "name", "")

            if name == "barrier":
                continue
            if name in {"measure", "reset"} or cargs:
                raise ValueError(
                    "noise_circuit must not contain measurements, resets or "
                    "classical-condition operations."
                )

            qindices = [circuit.find_bit(q).index for q in qargs]
            layer.append(operation, qindices)

            is_unitary = self._operation_is_unitary(operation)
            if is_unitary and not seen_noise:
                ideal.append(operation, qindices)
            elif not is_unitary:
                seen_noise = True
                noise.append(operation, qindices)
            else:
                raise ValueError(
                    "A unitary gate appears after a non-unitary noise channel. "
                    "This class expects a unitary Clifford prefix followed by "
                    "trailing noise-channel instructions."
                )

        if len(ideal.data) == 0:
            raise ValueError("No unitary layer was found in noise_circuit.")
        return layer, ideal, noise

    def _validate_clifford_layer(self) -> None:
        try:
            self._clifford = Clifford(self._ideal_layer)
        except Exception as exc:
            raise ValueError(
                "The ideal unitary portion of noise_circuit must be Clifford "
                "for automatic Pauli propagation."
            ) from exc

    def _infer_neighbor_edges(self) -> List[Tuple[int, int]]:
        edges = set()
        for item in self._ideal_layer.data:
            _, qargs, _ = self._instruction_parts(item)
            if len(qargs) != 2:
                continue
            q0 = self._ideal_layer.find_bit(qargs[0]).index
            q1 = self._ideal_layer.find_bit(qargs[1]).index
            edge = tuple(sorted((q0, q1)))
            if self.nearest_only and abs(edge[0] - edge[1]) != 1:
                continue
            edges.add(edge)
        return sorted(edges)

    def _pauli_label_from_ops(self, operations: Dict[int, str]) -> str:
        return "".join(
            operations.get(q, "I") for q in reversed(range(self.n_qubits))
        )

    def _build_noise_basis(self) -> List[str]:
        terms: List[str] = []
        axes = "XYZ"

        for q in range(self.n_qubits):
            for axis in axes:
                terms.append(self._pauli_label_from_ops({q: axis}))

        for qa, qb in self._edges:
            for axis_a in axes:
                for axis_b in axes:
                    terms.append(
                        self._pauli_label_from_ops({qa: axis_a, qb: axis_b})
                    )

        return terms

    @staticmethod
    def _anticommutes(label_a: str, label_b: str) -> bool:
        mismatch_count = 0
        for a, b in zip(label_a, label_b):
            if a != "I" and b != "I" and a != b:
                mismatch_count += 1
        return mismatch_count % 2 == 1

    def _build_direct_matrix(self) -> np.ndarray:
        M = np.array(
            [
                [
                    int(self._anticommutes(benchmark, noise_term))
                    for noise_term in self._model_terms
                ]
                for benchmark in self._benchmark_paulis
            ],
            dtype=float,
        )
        if np.linalg.matrix_rank(M) < len(self._model_terms):
            raise RuntimeError(
                "The automatically generated direct benchmark matrix is rank "
                "deficient. Use a richer Pauli basis or set nearest_only=False."
            )
        return M

    @staticmethod
    def _pauli_label_and_sign(pauli: Pauli) -> Tuple[str, int]:
        phase = int(pauli.phase) % 4
        if phase == 0:
            sign = 1
        elif phase == 2:
            sign = -1
        else:
            raise ValueError(
                "A Clifford-conjugated Hermitian Pauli unexpectedly acquired "
                "a +/-i phase."
            )
        label = "".join(char for char in pauli.to_label() if char in "IXYZ")
        return label, sign

    def _partner_pauli(self, pauli_label: str) -> str:
        evolved = Pauli(pauli_label).evolve(self._clifford, frame="s")
        label, _ = self._pauli_label_and_sign(evolved)
        return label

    def _preimage_pauli(self, pauli_label: str) -> Tuple[str, int]:
        evolved = Pauli(pauli_label).evolve(self._clifford, frame="h")
        return self._pauli_label_and_sign(evolved)

    def _build_pair_matrix(self) -> Tuple[np.ndarray, List[str]]:
        rows = []
        partners = []
        for benchmark, row in zip(self._benchmark_paulis, self._M):
            partner = self._partner_pauli(benchmark)
            partner_row = np.array(
                [
                    int(self._anticommutes(partner, noise_term))
                    for noise_term in self._model_terms
                ],
                dtype=float,
            )
            rows.append(row + partner_row)
            partners.append(partner)
        return np.asarray(rows, dtype=float), partners

    # ------------------------------------------------------------------
    # State preparation and measurement
    # ------------------------------------------------------------------
    def _local_pauli(self, label: str, qubit: int) -> str:
        return label[self.n_qubits - 1 - qubit]

    def _prepare_pauli_product_eigenstate(
        self, pauli_label: str, eigenvalue: int = 1
    ) -> QuantumCircuit:
        if eigenvalue not in (-1, 1):
            raise ValueError("eigenvalue must be +1 or -1.")

        circuit = QuantumCircuit(self.n_qubits, self.n_qubits)
        sign_assigned = False

        for q in range(self.n_qubits):
            operation = self._local_pauli(pauli_label, q)
            local_eigenvalue = 1
            if eigenvalue == -1 and not sign_assigned and operation != "I":
                local_eigenvalue = -1
                sign_assigned = True

            if operation == "X":
                if local_eigenvalue == -1:
                    circuit.x(q)
                circuit.h(q)
            elif operation == "Y":
                if local_eigenvalue == -1:
                    circuit.x(q)
                circuit.h(q)
                circuit.s(q)
            elif operation == "Z" and local_eigenvalue == -1:
                circuit.x(q)

        if eigenvalue == -1 and not sign_assigned:
            raise ValueError("The identity operator has no -1 eigenstate.")
        return circuit

    def _append_pauli_measurement(
        self, circuit: QuantumCircuit, pauli_label: str
    ) -> None:
        for q in range(self.n_qubits):
            operation = self._local_pauli(pauli_label, q)
            if operation == "X":
                circuit.h(q)
            elif operation == "Y":
                circuit.sdg(q)
                circuit.h(q)
        circuit.measure(range(self.n_qubits), range(self.n_qubits))

    def _append_pauli_correction(
        self, circuit: QuantumCircuit, pauli_label: str
    ) -> None:
        for q in range(self.n_qubits):
            operation = self._local_pauli(pauli_label, q)
            if operation == "X":
                circuit.x(q)
            elif operation == "Y":
                circuit.y(q)
            elif operation == "Z":
                circuit.z(q)

    def _append_forward_noisy_layer(self, circuit: QuantumCircuit) -> None:
        circuit.compose(self._ideal_layer, inplace=True)
        if len(self._noise_suffix.data):
            circuit.compose(self._noise_suffix, inplace=True)

    def _append_inverse_noisy_layer(self, circuit: QuantumCircuit) -> None:
        circuit.compose(self._ideal_layer.inverse(), inplace=True)
        if len(self._noise_suffix.data):
            circuit.compose(self._noise_suffix, inplace=True)

    def _make_echo_circuit(self, pauli_label: str, depth: int) -> QuantumCircuit:
        circuit = self._prepare_pauli_product_eigenstate(pauli_label, 1)
        for _ in range(int(depth)):
            self._append_forward_noisy_layer(circuit)
            self._append_inverse_noisy_layer(circuit)
        self._append_pauli_measurement(circuit, pauli_label)
        return circuit

    def _make_direct_circuit(
        self, pauli_label: str
    ) -> Tuple[QuantumCircuit, str, int]:
        preimage_label, preimage_sign = self._preimage_pauli(pauli_label)
        circuit = self._prepare_pauli_product_eigenstate(
            preimage_label, preimage_sign
        )
        self._append_forward_noisy_layer(circuit)
        self._append_pauli_measurement(circuit, pauli_label)
        return circuit, preimage_label, preimage_sign

    def _make_pec_circuit(self, selected_terms: Sequence[str]) -> QuantumCircuit:
        circuit = QuantumCircuit(self.n_qubits, self.n_qubits)
        circuit.compose(self._layer_circuit, inplace=True)
        for pauli_label in selected_terms:
            self._append_pauli_correction(circuit, pauli_label)
        self._append_pauli_measurement(circuit, self.observable)
        return circuit

    # ------------------------------------------------------------------
    # Execution and fitting
    # ------------------------------------------------------------------
    def _run_backend(
        self, circuits: Sequence[QuantumCircuit], shots: int, seed_offset: int = 0
    ) -> Any:
        run_options: Dict[str, Any] = {"shots": int(shots)}
        if self.seed is not None:
            run_options["seed_simulator"] = int(
                (self.seed + seed_offset) % (2**32 - 1)
            )
        try:
            return self.backend.run(list(circuits), **run_options).result()
        except TypeError:
            run_options.pop("seed_simulator", None)
            return self.backend.run(list(circuits), **run_options).result()

    def _bitstring_eigenvalue(self, bitstring: str, pauli_label: str) -> int:
        bits = bitstring.replace(" ", "")
        eigenvalue = 1
        for q in range(self.n_qubits):
            if self._local_pauli(pauli_label, q) != "I":
                bit = int(bits[-1 - q])
                eigenvalue *= -1 if bit else 1
        return eigenvalue

    def _counts_statistics(
        self, counts: Dict[str, int], pauli_label: str
    ) -> Tuple[float, int]:
        total = int(sum(counts.values()))
        signed_sum = 0
        for bitstring, count in counts.items():
            signed_sum += self._bitstring_eigenvalue(bitstring, pauli_label) * int(
                count
            )
        return float(signed_sum / total), total

    def _run_pair_benchmarks(self) -> np.ndarray:
        depths = np.arange(self.repeat + 1, dtype=int)
        circuits: List[QuantumCircuit] = []
        metadata: List[Tuple[str, int]] = []

        for pauli_label in self._benchmark_paulis:
            for depth in depths:
                circuits.append(self._make_echo_circuit(pauli_label, int(depth)))
                metadata.append((pauli_label, int(depth)))

        result = self._run_backend(circuits, shots=self.shots, seed_offset=11)
        ev_map: Dict[str, List[float]] = {
            p: [] for p in self._benchmark_paulis
        }
        for index, (pauli_label, _) in enumerate(metadata):
            ev, _ = self._counts_statistics(
                result.get_counts(index), pauli_label
            )
            ev_map[pauli_label].append(ev)

        pair_eta = np.zeros(len(self._benchmark_paulis), dtype=float)
        self._pair_results = []

        for index, pauli_label in enumerate(self._benchmark_paulis):
            ev_values = np.asarray(ev_map[pauli_label], dtype=float)
            if abs(ev_values[0]) < 1e-12:
                raise RuntimeError(
                    f"Zero reference expectation for benchmark {pauli_label}."
                )

            normalized = ev_values / ev_values[0]
            valid = np.isfinite(normalized) & (normalized > 0)
            if int(np.sum(valid)) < 2:
                raise RuntimeError(
                    f"Not enough positive decay points to fit {pauli_label}."
                )

            x = depths[valid].astype(float)
            y = np.log(normalized[valid])
            slope, intercept = np.polyfit(x, y, 1)
            predicted = intercept + slope * x
            ss_res = float(np.sum((y - predicted) ** 2))
            ss_tot = float(np.sum((y - np.mean(y)) ** 2))
            r_squared = 1.0 if ss_tot == 0 else 1.0 - ss_res / ss_tot

            fidelity_pair = float(np.clip(np.exp(slope), 1e-12, 1.0))
            eta_pair = float(max(0.0, -0.5 * math.log(fidelity_pair)))
            pair_eta[index] = eta_pair

            self._pair_results.append(
                {
                    "pauli": pauli_label,
                    "partner": self._partner_paulis[index],
                    "depths": depths.copy(),
                    "expectations": ev_values,
                    "normalized_expectations": normalized,
                    "slope": float(slope),
                    "intercept": float(intercept),
                    "r_squared": float(r_squared),
                    "fidelity_pair": fidelity_pair,
                    "eta_pair": eta_pair,
                }
            )

        return pair_eta

    def _select_rank_completing_direct_paulis(
        self,
    ) -> Tuple[List[str], np.ndarray]:
        augmented = self._M_pair.copy()
        current_rank = int(np.linalg.matrix_rank(augmented))
        unknowns = augmented.shape[1]
        remaining = list(range(len(self._benchmark_paulis)))
        selected: List[int] = []

        while current_rank < unknowns:
            best_index = None
            best_rank = current_rank
            for index in remaining:
                candidate = np.vstack([augmented, self._M[index]])
                candidate_rank = int(np.linalg.matrix_rank(candidate))
                if candidate_rank > best_rank:
                    best_rank = candidate_rank
                    best_index = index

            if best_index is None:
                raise RuntimeError(
                    "No direct-fidelity row can complete the benchmark rank."
                )

            augmented = np.vstack([augmented, self._M[best_index]])
            selected.append(best_index)
            remaining.remove(best_index)
            current_rank = best_rank

        selected_labels = [self._benchmark_paulis[i] for i in selected]
        return selected_labels, augmented

    def _run_direct_benchmarks(self, selected: Sequence[str]) -> np.ndarray:
        if not selected:
            self._direct_results = []
            return np.empty(0, dtype=float)

        circuits = []
        metadata = []
        for pauli_label in selected:
            circuit, preimage_label, preimage_sign = self._make_direct_circuit(
                pauli_label
            )
            circuits.append(circuit)
            metadata.append((pauli_label, preimage_label, preimage_sign))

        result = self._run_backend(circuits, shots=self.shots, seed_offset=29)
        eta = np.zeros(len(selected), dtype=float)
        self._direct_results = []

        for index, (pauli_label, preimage_label, preimage_sign) in enumerate(
            metadata
        ):
            expectation, _ = self._counts_statistics(
                result.get_counts(index), pauli_label
            )
            fidelity = float(np.clip(expectation, 1e-12, 1.0))
            eta_direct = float(-0.5 * math.log(fidelity))
            eta[index] = eta_direct
            self._direct_results.append(
                {
                    "Target Pauli": pauli_label,
                    "Prepared Pauli": preimage_label,
                    "Prepared eigenvalue": int(preimage_sign),
                    "fidelity_direct": fidelity,
                    "eta_direct": eta_direct,
                }
            )

        return eta

    # ------------------------------------------------------------------
    # QPD and PEC execution
    # ------------------------------------------------------------------
    def _build_qpd(self) -> None:
        if not self._fit_complete:
            raise RuntimeError("fit() must complete before QPD construction.")

        self._qpd_terms = []
        for pauli_label, rate in zip(self._model_terms, self._learned_lambdas):
            rate = float(max(0.0, rate))
            if rate <= self.cutoff:
                continue
            p_pauli = 0.5 * (1.0 - math.exp(-2.0 * rate))
            self._qpd_terms.append(
                {
                    "pauli": pauli_label,
                    "lambda": rate,
                    "p_identity": float(1.0 - p_pauli),
                    "p_pauli": float(p_pauli),
                }
            )

        total_rate = float(sum(term["lambda"] for term in self._qpd_terms))
        self._gamma = float(math.exp(2.0 * total_rate))
        self._qpd_complete = True

    def _qpd_set_probability(self, selected_terms: Sequence[str]) -> float:
        selected = set(selected_terms)
        probability = 1.0
        for term in self._qpd_terms:
            probability *= (
                term["p_pauli"]
                if term["pauli"] in selected
                else term["p_identity"]
            )
        return float(probability)

    def _execute_sampled_qpd_circuits(self) -> None:
        if not self._sample_complete:
            self.sample_qpd_sets()

        # Build temporary circuits when store_circuits=False.
        if self.store_circuits:
            records = self._qpd_circuit_records
        else:
            records = [
                {
                    **record,
                    "circuit": self._make_pec_circuit(record["selected_terms"]),
                }
                for record in self._qpd_records
            ]

        records_by_count: Dict[int, List[Dict[str, Any]]] = defaultdict(list)
        for record in records:
            records_by_count[int(record["count"])].append(record)

        total_measurements = 0
        weighted_sum = 0.0
        squared_sum = 0.0
        execution_rows: List[Dict[str, Any]] = []

        seed_offset = 100
        for multiplicity, group in records_by_count.items():
            circuits = [record["circuit"] for record in group]
            result = self._run_backend(
                circuits, shots=multiplicity, seed_offset=seed_offset
            )
            seed_offset += 1

            for index, record in enumerate(group):
                expectation, actual_shots = self._counts_statistics(
                    result.get_counts(index), self.observable
                )
                weight = float(record["weight"])
                weighted_sum += weight * expectation * actual_shots
                squared_sum += (weight**2) * actual_shots
                total_measurements += actual_shots

                execution_rows.append(
                    {
                        "QPD set": record["QPD set"],
                        "count": actual_shots,
                        "empirical_probability": record[
                            "empirical_probability"
                        ],
                        "theoretical_probability": record[
                            "theoretical_probability"
                        ],
                        "sign": record["sign"],
                        "weight": weight,
                        "observable_ev": float(expectation),
                        "weighted_contribution": float(
                            weight * expectation * actual_shots / self.shots
                        ),
                    }
                )

        if total_measurements == 0:
            raise RuntimeError("No PEC measurement results were returned.")

        mean = float(weighted_sum / total_measurements)
        if total_measurements > 1:
            sample_variance = float(
                max(
                    0.0,
                    (squared_sum - total_measurements * mean**2)
                    / (total_measurements - 1),
                )
            )
            standard_error = float(
                math.sqrt(sample_variance / total_measurements)
            )
        else:
            sample_variance = 0.0
            standard_error = 0.0

        self._result = {
            "observable": self.observable,
            "pec_expectation": mean,
            "variance": sample_variance,
            "standard_error": standard_error,
            "overhead": float(self._gamma),
            "sampling_cost_factor": float(self._gamma**2),
            "shots": int(total_measurements),
            "unique_qpd_sets": len(execution_rows),
            "qpd_execution_table": pd.DataFrame(execution_rows),
        }
        self._run_complete = True

        if not self.store_circuits:
            records.clear()

    # ------------------------------------------------------------------
    # Lazy-state/reset helpers
    # ------------------------------------------------------------------
    def _ensure_qpd(self) -> None:
        if not self._fit_complete:
            self.fit()
        elif not self._qpd_complete:
            self._build_qpd()

    def _reset_after_fit(self) -> None:
        self._fit_complete = False
        self._qpd_complete = False
        self._sample_complete = False
        self._run_complete = False
        self._pair_results = []
        self._direct_results = []
        self._selected_direct_paulis = []
        self._M_fit = None
        self._eta_fit = None
        self._learned_lambdas = None
        self._nnls_residual = None
        self._qpd_terms = []
        self._gamma = None
        self._qpd_records = []
        self._qpd_circuit_records = []
        self._result = None

    def _reset_all_results(self) -> None:
        self._reset_after_fit()


__all__ = ["PEC"]
