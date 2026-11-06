# Tullius Novelist (툴리우스 소설가)

> 상태: **설계 단계, 아직 배포 전**. 코드는 0단계 검증 실험부터 들어갑니다.

평소처럼 스카이림을 하고 저장만 하면, 그동안의 모험을 AI가 장편 소설로 엮어 주는 Skyrim SE/AE용 SKSE 모드입니다. 완성된 장은 게임 안에서 책으로 읽습니다.

- **게임 안 무간섭**: 플레이 중에는 아무것도 묻지 않습니다. 저장할 때마다 그 사이의 활동이 기록됩니다.
- **세세한 기록**: 공격, 피해, 막기, 회피, 포션, 주문, 은신, 거래까지 기록해서 그 사람만의 이야기를 만듭니다.
- **세이브 계보**: 예전 세이브를 불러와 다시 한 진행이 정사가 되고, 버린 진행은 외전 재료로 남습니다.
- **쓰고 있는 AI 구독 그대로**: Claude, ChatGPT(Codex), Gemini(Antigravity) 구독 앱에서 "작가 키트" 폴더를 열고 "내 이야기 이어서 써 줘"라고 하면, 앱의 AI가 로컬 도구로 기록을 읽어 소설을 씁니다. API 키는 필요 없습니다. 모드는 AI 앱을 몰래 실행하지 않습니다.
- **언어**: 한국어를 먼저 지원하고, 영어를 이어서 지원합니다.

## 문서

| 문서 | 내용 |
|---|---|
| [docs/design-draft.md](docs/design-draft.md) | 설계 초안 (현재 v0.4) |
| [docs/activity-catalog.md](docs/activity-catalog.md) | 기록할 플레이 활동 전체 목록과 결정 |
| [docs/research-verification-2026-11-06.md](docs/research-verification-2026-11-06.md) | 조사 검증 기록 |
| [docs/reviews/](docs/reviews/) | 교차 검토(Codex) 원문과 정리 |

## English (short)

Tullius Novelist is a planned SKSE plugin for Skyrim SE/AE that records your playthrough in fine detail (combat, survival, stealth, trade, quests) along your save lineage, and lets the AI app you already subscribe to (Claude Code, Codex, Antigravity) turn it into a novel through a local MCP tool. Finished chapters are readable in game. Korean first, English next. Currently in the design phase.

## 라이선스

GPL-3.0-or-later. [LICENSE](LICENSE)를 보세요. SKSE 플러그인은 GPL-3.0인 [CommonLibSSE-NG](https://github.com/alandtse/CommonLibSSE-NG)를 사용합니다.

## 툴리우스 시리즈

[Tullius CTD Logger](https://www.nexusmods.com/skyrimspecialedition/mods/172272) · [Tullius Widgets](https://www.nexusmods.com/skyrimspecialedition/mods/172458) · [툴리우스 번역기](https://github.com/servaltullius/Skyrim_Translator_v6)
