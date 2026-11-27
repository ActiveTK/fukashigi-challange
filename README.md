# fukashigi-challange

OEIS A007764（n×nマスの左上の角から右下の角まで、同じ点を2度通らない道順の数）の27×27マスの値を、GPU向けのフロンティア法で計算しました。

- 答え: [results/27/answer.txt](results/27/answer.txt)（177桁、10進表記・改行なしのSHA-256は `9884d61a1d3db8e404debdac6c7660c0c400a0e241acaddf1b8d0aa55433a316`）
- 38個の16bit素数での余りと実行記録: [results/27/res.txt](results/27/res.txt)
- 余りから組み立て直して確かめる: `make && ./build/fukashigi-cpu --crt results/27/res.txt`（出力は [results/27/report.txt](results/27/report.txt) と同じになります）
