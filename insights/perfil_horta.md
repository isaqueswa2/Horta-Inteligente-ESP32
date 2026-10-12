# Perfil da horta

Este arquivo é lido pela IA a cada geração de insights. Edite à vontade: quanto mais
exato o que está aqui, melhores e mais úteis ficam as análises. Os trechos marcados
com **(confirme)** são suposições minhas: corrija ou apague.

## O sistema
- Horta hidropônica no Sul do Brasil. Clima subtropical úmido, verões quentes.
  (Este arquivo é público: não escreva endereço, cidade ou coordenadas aqui.)
- Bomba em ciclos de liga/desliga controlada por uma tomada inteligente; a duração dos
  ciclos está nos ajustes enviados com os dados.
- Sensores: temperatura da solução (DS18B20), vazão da bomba (YF-S201), nível mínimo do
  reservatório (liga/desliga: só avisa quando chega ao mínimo, não mede o volume).
- A temperatura **ambiente** não vem de um sensor local: é a previsão atual do Open-Meteo
  para a localização da horta. Pode diferir do que a horta sente de verdade.
- Não há sensor de pH nem de condutividade (EC). Não opine sobre pH/EC como se fossem
  medidos; no máximo, sugira conferir manualmente quando fizer sentido.
- A horta e o reservatório ficam na varanda, expostos ao sol. O reservatório é fechado: a
  chuva não entra na solução e não a dilui. Só as plantas e os canais ficam sujeitos à chuva.
- Sistema NFT (canais com lâmina fina de solução).

## Culturas e faixas de referência
Faixas gerais de literatura para a temperatura da **solução nutritiva**; use como
referência, sem tratar como lei. Todas as culturas dividem o mesmo reservatório, então
vale o equilíbrio entre elas.

| Cultura | Solução ideal | Observações |
|---|---|---|
| Alface | 18–24 °C | Acima de ~26 °C estressa; calor prolongado no ar (>30 °C) causa pendoamento e amargor |
| Rúcula | 15–22 °C | Sensível ao calor: pendoa e fica picante/amarga |
| Hortelã | 18–26 °C | Tolerante; cresce muito, pode competir por nutrientes |
| Salsinha | 16–24 °C | Tolera fresco; desenvolvimento lento |
| Cebolinha | 15–25 °C | Rústica |
| Manjericão | 20–28 °C | Sensível ao frio: abaixo de ~15 °C na solução ou noites <12 °C causam danos |
| Manjerona | 18–28 °C | Prefere menos umidade na raiz |
| Orégano | 18–28 °C | Prefere menos umidade na raiz; tolera calor |
| Stévia | 20–30 °C | Sensível ao frio (<10 °C); gosta de muita luz |

Pontos transversais:
- Solução acima de ~26–28 °C: menos oxigênio dissolvido e risco maior de podridão de raiz
  (Pythium). É o principal risco do verão.
- Solução abaixo de ~15 °C: o crescimento desacelera e manjericão/stévia sofrem.
- Queda gradual de vazão com a bomba ligada sugere entupimento de filtro/canais ou
  desgaste da bomba.
- O consumo de água acompanha a evapotranspiração (ET0): dias quentes, secos e ensolarados
  esvaziam o reservatório mais rápido.
- Solução nutritiva costuma ser renovada a cada 1 a 2 semanas (ajuste pela sua prática).

## O que eu quero receber
- Insights curtos, práticos e específicos para a minha horta.
- Prioridade: riscos (calor, frio, entupimento, falta de água) e o que fazer a respeito.
- Português do Brasil, tom direto e amigável, sem enrolação.
