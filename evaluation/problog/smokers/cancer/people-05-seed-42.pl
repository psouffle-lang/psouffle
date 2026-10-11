% Benchmark setting: {"case":"smokers-cancer-5","evidence_atoms":5,"family":"fierens2015-smokers","fidelity":"paper model; reconstructed graph and random instances","graph":{"algorithm":"Barabasi-Albert; initial clique; symmetric friends","m":2},"independent_events":29,"input_rows":{"ask_cancer":4,"ask_smokes":1,"friend":14,"influence_occ":14,"person":5,"smoking_cancer_occ":5,"spontaneous_occ":5,"stress_occ":5},"input_sha256":{"ask_cancer.facts":"c8c2fa3f3381dbb6a25967b8453aba2d4d5bc293d45e78a4e488cad330a2d9cf","ask_smokes.facts":"b9f5f5b99acec3f75c23a4dfd4118105902523f9ecfacb4c24b895c7299fb9fc","event.facts":"722b457e4418944bb24e5c8606c0c8bca8ac0248c601a840e5c93c9db15e8639","event.prob":"f0b2048459d59a576566c54356ad4a01d794d02eece9a58aaa8eba4d71034757","friend.facts":"b0366eb3ae629e062373867d1d217f7f7fd1427f7da7f79165676083573e8c7d","influence_occ.facts":"1f6c257cb01d010403e03ebcfbb17b1372f0b9dfe9e7484cbc0ca7a88ab68d31","person.facts":"21dda906788069b06a376b4536855c7b8426e9a42fb5fcbdfffb0da4812eaeae","smoking_cancer_occ.facts":"b32c23c61bb4ff7dbfa820cc4103b55735f11f25afd072350cecf4e25e7ec4d3","spontaneous_occ.facts":"c9a568e7ad14ce5d4312086d1d44793e3cb6fdce049b12b286d0abd0cae54f08","stress_occ.facts":"26931f0b2cc80b22f3dd00c3f41df720c8e0ac168d3eb135ce829b2477e38674"},"original_compute_dl_sha256":"1ed0dd34a512602451f05d212c74efd3462041a191b74e115552cc44471a84c0","original_compute_pl_sha256":"7b3c2d510817158668c61c3aad37a18597ff7daf4fb17b4a5b4cf7a1112222e3","outputs":["q_cancer","q_smokes"],"people":5,"query_atoms":5,"seed":42,"semantics":"exact; no depth bound"}
:- unknown(fail).
person('p0').
person('p1').
person('p2').
person('p3').
person('p4').
friend('p0','p1').
friend('p0','p2').
friend('p0','p3').
friend('p0','p4').
friend('p1','p0').
friend('p1','p2').
friend('p1','p3').
friend('p2','p0').
friend('p2','p1').
friend('p2','p4').
friend('p3','p0').
friend('p3','p1').
friend('p4','p0').
friend('p4','p2').
stress_occ('p0','e0').
stress_occ('p1','e1').
stress_occ('p2','e2').
stress_occ('p3','e3').
stress_occ('p4','e4').
spontaneous_occ('p0','e5').
spontaneous_occ('p1','e6').
spontaneous_occ('p2','e7').
spontaneous_occ('p3','e8').
spontaneous_occ('p4','e9').
smoking_cancer_occ('p0','e10').
smoking_cancer_occ('p1','e11').
smoking_cancer_occ('p2','e12').
smoking_cancer_occ('p3','e13').
smoking_cancer_occ('p4','e14').
influence_occ('p0','p1','e15').
influence_occ('p0','p2','e16').
influence_occ('p0','p3','e17').
influence_occ('p0','p4','e18').
influence_occ('p1','p0','e19').
influence_occ('p1','p2','e20').
influence_occ('p1','p3','e21').
influence_occ('p2','p0','e22').
influence_occ('p2','p1','e23').
influence_occ('p2','p4','e24').
influence_occ('p3','p0','e25').
influence_occ('p3','p1','e26').
influence_occ('p4','p0','e27').
influence_occ('p4','p2','e28').
ask_cancer('p0').
ask_cancer('p2').
ask_cancer('p3').
ask_cancer('p4').
ask_smokes('p0').
0.2::event('e0').
0.2::event('e1').
0.2::event('e2').
0.2::event('e3').
0.2::event('e4').
0.1::event('e5').
0.1::event('e6').
0.1::event('e7').
0.1::event('e8').
0.1::event('e9').
0.3::event('e10').
0.3::event('e11').
0.3::event('e12').
0.3::event('e13').
0.3::event('e14').
0.3::event('e15').
0.3::event('e16').
0.3::event('e17').
0.3::event('e18').
0.3::event('e19').
0.3::event('e20').
0.3::event('e21').
0.3::event('e22').
0.3::event('e23').
0.3::event('e24').
0.3::event('e25').
0.3::event('e26').
0.3::event('e27').
0.3::event('e28').
stress(X) :- stress_occ(X,I), event(I).
influences(X,Y) :- influence_occ(X,Y,I), event(I).
cancer_spont(X) :- spontaneous_occ(X,I), event(I).
cancer_smoke(X) :- smoking_cancer_occ(X,I), event(I).
smokes(X) :- stress(X).
smokes(X) :- smokes(Y), influences(Y,X).
cancer(X) :- cancer_spont(X).
cancer(X) :- smokes(X), cancer_smoke(X).
q_cancer(X0) :- ask_cancer(X0), cancer(X0).
q_smokes(X0) :- ask_smokes(X0), smokes(X0).
query(q_cancer(_)).
query(q_smokes(_)).
evidence(cancer('p1'),false).
evidence(smokes('p1'),false).
evidence(smokes('p2'),false).
evidence(smokes('p3'),false).
evidence(smokes('p4'),true).
