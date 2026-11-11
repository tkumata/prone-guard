# 2026-11-09 寝返り検知の改修と評価

## 目的

寝返りを検知できない原因を特定し、時系列の根拠をJevへ提供する。
検知精度は実測値に基づいて評価する。

## 実装済み

- 観測品質の失敗理由を判定結果の `quality` に保持する。
- 顔矩形、顔検出信頼度、ランドマークを任意項目として保存・送信する。
- 深度の有効セル数、中央値、セル重心、隣接勾配を時系列で算出する。
- 深度変化の候補について前後10秒間の観測を比較する。
- Jevには元の観測値と特徴量を渡し、胴体の向きの変化を検証させる。
- Jevの生の回答と最終判断の理由を保持する。
- 評価プログラムで検知率、誤検知率、判定不能率、検知遅延を計算する。

## 評価データ

人形などで実験し、180秒の判定窓ごとに正解をCSVへ記録する。
CSVの列は `bed_id,window_start,window_end_exclusive,label,event_at` とする。
`label` は `rollover` または `no_rollover`。
`rollover` ではその窓内の最初の寝返り時刻を `event_at` に指定する。
エッジの標準エラー出力にある `posture_result` JSONL を保存する。

```sh
python3 tools/evaluate_rollover.py labels.csv edge.log.jsonl
python3 -m unittest discover -s tests -p test_evaluate_rollover.py
```

出力する `recall_conservative` は判定不能を未検知として数える。
`false_positive_rate_conservative` の分母は正解が寝返りなしの全窓である。
`precision_decided` と `recall_decided` は判定できた窓だけの参考値とする。
実行ログが欠けた窓は判定不能に分類する。
検知遅延には180秒周期の待ち時間も含む。

## 限界と残作業

- 8×8の深度だけでは胴体の向きを確定できない場合がある。
- 顔の移動や深度変化は寝返りの確定的な証拠ではない。
- 実測の精度数値はラベル付きデータがないため未算出。
- ESP-IDF実機の稼働、Jev APIの判定性能、計測データの整合は別途検証する。
- 暫定の距離閾値を実測値で調整し、改善前後を同じデータで比較する。
