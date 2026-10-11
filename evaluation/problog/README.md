# ProbLog Literature Benchmarks

Fixed inference inputs are supplied in paired ProbLog (`.pl`) and PSouffle
(`.dl`) formats. Facts, probabilities, queries and evidence are embedded.
Genes and Grid share a `network.pl` / `network.dl` within their directories;
run a query/start file. No conversion script or external fact directory is
required. These workloads are separate from the CAV/FMCAD paper experiments.

## Benchmark Settings

| Family | Case basenames | Setting | Origin |
| --- | --- | --- | --- |
| Genes | `genes/query-01` … `query-50`; 50 cases | 5,173 endpoints; 15,147 protein–protein and 5,568 protein–DNA occurrences; 20,715 independent events; one fixed gene-pair query | Imported vProbLog inputs |
| Smokers/asthma | `smokers/asthma/<upstream-id>`; 70 cases | 14–20 people, ten networks per size; stress 0.3, influence 0.2, asthma cause 0.4; all smoking/asthma marginals; no evidence | Imported vProbLog inputs |
| Smokers/cancer | `smokers/cancer/people-05-seed-42`, `people-10-seed-43`, `people-20-seed-44`; 3 cases | Stress 0.2, influence 0.3, spontaneous cancer 0.1, smoking-induced cancer 0.3; half the smoking/cancer atoms queried and half observed | Reconstructed Fierens 2015 instances |
| Grid | `grid/start-01` … `start-15`; 15 cases | 16×16 directed grid; 705 independent edges of probability 0.5; query `(i,i)` → `(16,16)` | Reconstructed published topology and queries |
| WebKB | `webkb/cornell-10`, `cornell-25`, `cornell-50`; 3 cases | 10/25/50 Cornell pages; seven classes; 46 selected words; recursive link propagation; sampled evidence; fixed learned weights | Reconstructed model using Alchemy data |

There are **141 cases**, each in both formats. The two shared network pairs
give **286 `.pl` / `.dl` files** in total. Case headers record settings, input
counts and SHA-256 hashes of the programs and input files before packaging.
All cases request exact possible-world inference with positive recursion
interpreted as its least fixed point, without a path-depth bound.

### Genes

Inputs come from `scenarios/genes-1` … `genes-50` in the
[vProbLog snapshot](https://github.com/jjjxia/Vproblog/tree/ecf5d8b6d2c7576b2e54ad2f59bd0beb8887a70a/scenarios)
at commit `ecf5d8b6d2c7576b2e54ad2f59bd0beb8887a70a`, a mirror/fork of
[Tsamoura's artifact](https://bitbucket.org/tsamoura/vproblog/) for
[Beyond the Grounding Bottleneck, AAAI 2020](https://ojs.aaai.org/index.php/AAAI/article/view/6591).

Each occurrence retains its original event ID. Parallel interactions remain
independent; both directions of one protein–protein interaction share its
single event. Protein–DNA interactions are directed. Probabilities retain the
supplied mapping, including 0.96 for protein–DNA interactions.

The path base case is a protein–DNA edge. Recursion prepends an interaction and
requires `X != Y`, so this differs from unrestricted reachability. Deterministic
magic bindings restrict grounding without conditioning on random edge outcomes.
Each case requests `q(source,target)` through its single `mgc_q` fact.

The network originates from
[Ourfali et al.'s SPINE task](https://idekerlab.ucsd.edu/wp-content/uploads/publications/Ourfali_Bioinformatics_ISMB-ECCB_2007.pdf).
That paper reports 5,313 nodes, whereas the downloaded edge endpoints contain
5,173 distinct IDs; this discrepancy remains unresolved. Query pairs are the
50 downloaded cases. Earlier papers use other query sets and bounded paths;
AAAI 2020 reports ProbLog lower bounds under a time limit. These files request
completed exact probabilities.

### Smokers/asthma

All 70 networks retain friendships, events and probabilities from
`scenarios/smokers-<upstream-id>` in the pinned vProbLog snapshot. IDs are
upstream names; their middle number is not a verified friendship count.

```prolog
smokes(X) :- stress(X,I), event(I).
smokes(X) :- friend(X,Y), influences(Y,X,I), event(I), smokes(Y).
asthma(X) :- smokes(X), smoking_causes_asthma(X,I), event(I).
```

Friendships are symmetric without self-loops. Influence events cover every
ordered pair, including self-pairs, but only friend pairs participate in the
rule. All `n*n + 2*n` source events are retained, including unused influences.
Each case queries `n` smoking and `n` asthma marginals, without evidence.
The upstream rules enable only smoking output and comment out asthma output;
these cases enable **both**, using the supplied asthma causes.

The downloaded snapshot has sizes 14–20; sizes 10–13 discussed in AAAI 2020
were unavailable. Original graph-generation code and seeds were not recovered.

### Smokers/cancer

These cases implement the model and evidence protocol in
[Fierens et al. 2015, Section 9](https://starai.cs.ucla.edu/papers/FierensTPLP15.pdf).
They are new random instances; the authors' original networks and seeds were
not recovered. Generation used Barabási–Albert preferential attachment with
`m=2`, an initial three-person clique, and two undirected edges per added person.
Each friendship yields two independent directed influences. Graph generation,
evidence splitting and possible-world sampling used separate seeded streams.

| People | Seed | Undirected friendships | Events | Queries | Evidence |
| --- | --- | --- | --- | --- | --- |
| 5 | 42 | 7 | 29 | 5 | 5 |
| 10 | 43 | 17 | 64 | 10 | 10 |
| 20 | 44 | 37 | 134 | 20 | 20 |

Stress or a smoking influencer causes smoking. Spontaneous cancer or smoking
together with the smoking-induced cause produces cancer. Of the `2*n` smoking
and cancer atoms, exactly `n` are observed and `n` queried. Evidence comes from
a sampled world followed by least-fixed-point evaluation and therefore has
positive joint probability. Selected atoms and truth values are explicit in
the files. Outputs are `q_smokes` and `q_cancer`. The recursive benchmark
programs are retained; the diagnostic elimination script has been removed.

### Grid

Nodes are `(x,y)` for `x,y=1..16`. Edges go right, down and diagonally down-right
within the grid. Each of the 705 edges is independently present with probability
0.5. Each case uses one source `(i,i)`, `i=1..15`, and target `(16,16)`.
Deterministic relevance selects the reachable suffix of side length `17-i`
without changing its probability. There is one `answer` query and no evidence.
The `start-15` query has analytic probability **0.71875**.

### WebKB

The source is the [Alchemy WebKB archive](https://alchemy.cs.washington.edu/data/webkb/webkb-alchemy.tgz),
SHA-256 `6c4c46156dad66e2ff935f2c3cb261fe446530acf8d48290e87ca0809e096c5d`,
adapted from [CMU's relational data](https://www.cs.cmu.edu/~WebKB/ILP-data.html).
Original Fierens preprocessing, learned weights and instances were not recovered.
The packaged reconstruction fixes these choices:

- Keep positively labeled pages, seven label predicates including `Person`,
  and induced links; labels may overlap. Before subsampling, Cornell, Texas,
  Washington and Wisconsin have 272, 289, 285 and 362 pages respectively.
  Exclude 149 malformed external word records and four external Wisconsin links.
- Hold out Cornell. Rank words independently in the other three universities
  by binary word-presence information gain against the complete label set,
  breaking ties alphabetically. Union their top 20 words to obtain 46 features;
  Cornell labels do not participate in ranking.
- Fix weights from a **small training run**: one sampled page from each training
  university, seed 42, ten iterations of ProbLog 2.2.10 log-space LFI with SDD.
  Training uses word causes, learnable priors, a fixed 0.001 prior and one hop
  of influence from local labels. Parameters without instances retain 0.5.
- Inference samples 10/25/50 Cornell pages with seed 42 and uses word causes
  plus recursive link propagation, without class priors. Half the page/class
  atoms are evidence sampled from a model world. Preserve all weights,
  including zero values, without threshold pruning.

| Cornell pages | Induced links | Events | Queries | Evidence |
| --- | --- | --- | --- | --- |
| 10 | 0 | 763 | 35 | 35 |
| 25 | 5 | 2,184 | 88 | 87 |
| 50 | 16 | 4,501 | 175 | 175 |

Each word/page/class and link/class-pair occurrence is a separate event.
Equal parameter values do not imply shared events. Output is `q_has_class`.
These are exact inference workloads with fixed smoke-fit weights, without a
claim to reproduce full-scale learning or published timings.

### Alzheimer

No Alzheimer case is bundled. The missing target is the Biomine network in
[De Raedt et al. 2007, Section 6](https://www.cs.helsinki.fi/hannu.toivonen/pubs/IJCAI07-397.pdf):
5,220 nodes and 11,530 edges with biological weights and sampled connectivity
instances. Its historical snapshot, weights and instances remain unavailable.
Genes is a different biological network.

## Encoding And Validation

ProbLog has one probabilistic `event(ID)` fact per source event. PSouffle groups
deterministic event-ID facts by probability and uses one probabilistic rule per
distinct weight:

```souffle
.decl event(id:symbol)
.decl event_weight_00(id:symbol)
event_weight_00("e0").
event_weight_00("e1").
0.3::event(I) :- event_weight_00(I).
```

Each ground rule instance is independent; all uses of the same `event(ID)`
share its event. Occurrence tables are deterministic. Both formats preserve
queries and condition on joint evidence as `P(query AND evidence) / P(evidence)`.
Small probability annotations are written as decimal literals because the
PSouffle annotation parser does not accept scientific notation.

Before removing converters, all 619 downloaded files passed their pinned
checksums. All 141 cases were checked against prepared inputs for fact
multisets, event identities and double precision weights, rules, query
selection and evidence. Regeneration passed 157 archived input-hash checks.
All 141 PSouffle cases and 143 ProbLog files passed syntax checks. Two Grid,
two Smokers/cancer, one Smokers/asthma and one WebKB case matched plain/rewrite
against ProbLog SDD across 80 outputs (maximum difference `5e-9`). Twelve
historical probability comparisons also matched. A separate six-event Genes
fixture matched all 64 possible worlds, covering parallel events, shared reverse
edges, directed terminals, cycles and the recursion guard. Full Genes cases
received input/rule checks rather than completed full-network inference.

The WebKB correctness probe used a debug build after optimized native
compilation exceeded 180 seconds; this is recorded in the validation CSV and
does not constitute a speed comparison. Expensive unsolved cases are not
counted as completed probability comparisons.

The upstream Apache 2.0 notice is retained in
[licenses/VPROBLOG.txt](../../licenses/VPROBLOG.txt). The scenario directories
and Alchemy archive supply no separate dataset license. Raw downloads,
conversion/learning/diagnostic scripts, intermediate inputs and binaries have
been removed after packaging.

## Run And Compare

From the repository root, with PSouffle built and ProbLog/PySDD installed:

```bash
CASE=evaluation/problog/smokers/cancer/people-05-seed-42
RUN=$(mktemp -d)
mkdir -p "$RUN/plain" "$RUN/rewrite"
./build/src/souffle --full-only "$CASE.dl" -o "$RUN/compute"
OMP_NUM_THREADS=1 "$RUN/compute" -D "$RUN/plain"
OMP_NUM_THREADS=1 "$RUN/compute" --rewrite -D "$RUN/rewrite"
OMP_NUM_THREADS=1 OPENBLAS_NUM_THREADS=1 problog "$CASE.pl" -k sdd
rm -rf -- "$RUN"
```

Other basenames include `evaluation/problog/genes/query-01`,
`evaluation/problog/grid/start-15`, `evaluation/problog/smokers/asthma/14-28-5`
and `evaluation/problog/webkb/cornell-10`. Shared network paths resolve relative
to the selected program. No `-F` option is needed.

For comparisons, use the same case, one thread, a **120-second inference
timeout** and **4 GiB virtual-address-space limit** for each process. On Linux,
run inference in a subshell with `ulimit -v 4194304`, `ulimit -c 0` and
`timeout --kill-after=3s 120s`. Record native C++ compilation separately.
Inference time includes loading, grounding, rewrite when enabled, knowledge
compilation, counting and output. Reference comparisons use ProbLog 2.2.10 /
PySDD 1.0.6 exact SDD. Incomplete bounds do not count as solved. Compare all
requested tuples within absolute tolerance `1e-8`; absent PSouffle tuples are
zero.

Historical experiment CSVs and packaging validation are preserved outside this
benchmark tree under ignored `evaluation/full/artifact/problog-history/`.
Historical timings used external fact files; embedded inputs preserve semantics
but can change graph layout and event order. Rerun measurements for these files.
