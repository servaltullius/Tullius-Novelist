# 조사 검증 기록 (2026-11-06)

Codex 조사 보고서의 주장을 Claude가 직접 다시 확인한 결과다. 프롬프트는 [codex-research-prompt.md](codex-research-prompt.md)에 있다.

## 직접 재확인한 사실

| 항목 | 결과 | 근거 |
|---|---|---|
| 이벤트 헤더 | `QuestStatus::Event`, `LocationDiscovery::Event`가 있다. `LocationCleared::Event`는 payload가 비어 있다. | CommonLibSSE-NG 로컬 헤더 |
| 저널 텍스트 | `TESQuest::GetJournalTextForInstance(BSString&, instanceID)`가 있다. | TESQuest.h:245 |
| 이벤트 발송 | `BSTEventSource::SendEvent`가 spin lock을 잡은 채 sink를 동기 호출한다. 콜백은 가볍게 유지해야 한다. | BSTEvent.h:131 |
| 한국어 STRINGS | Skyrim_English.STRINGS의 30,301개 문자열이 모두 UTF-8로 디코딩된다. 이 중 26,600개에 한글이 들어 있다. | 파일 직접 파싱 |
| SexLab P+ 2.17.1 | `SendModEvent(HookEvent, thread_id)` 형태로 보낸다. thread ID는 `strArg`로 온다. | sslThreadModel.psc:1978 |
| CommonLibSSE-NG 라이선스 | GPL-3.0-or-later에 Modding Exception이 붙어 있다. 기존 MIT에서 바뀌었다. 이 라이브러리와 링크하는 플러그인은 GPL 호환 라이선스여야 한다. | README.md:222, 234–237 |
| Claude Code 옵션 | `--safe-mode`, `--disallowedTools`, `--tools`, `--no-session-persistence`, `--bare`, `--output-format`이 설치본 help에 있다. 공식 문서는 `--disallowedTools "*"`가 모든 도구를 제거한다고 명시한다. | `claude --help`, 공식 CLI 문서 |
| Codex CLI 옵션 | `exec`에 `--sandbox`, `--ephemeral`, `--ignore-user-config`, `--json`, `-o`, `--skip-git-repo-check`가 있다. 모든 도구를 한 번에 끄는 옵션은 없다. | `codex exec --help` |
| Claude 약관 | 서드파티 개발자가 사용자를 대신해 Free/Pro/Max/Ultra 자격으로 요청을 중개하는 것은 허용하지 않는다. 사용자가 수정하지 않은 공식 바이너리에 자기 구독으로 로그인하는 것은 허용한다. "제품에서 Claude Code를 실행하는 것"은 조건부로 허용한다. | code.claude.com/docs/en/legal-and-compliance |
| Gemini CLI | 2026-06-18부터 개인 무료와 Google AI Pro/Ultra 접근이 Antigravity CLI로 옮겨졌다. gemini CLI에는 API 키, Vertex, 조직 라이선스 경로만 남았다. | gemini-cli README, Google 블로그 |
| Nexus AI 정책 | 표기는 AI-Generated Content / AI Media / AI Assisted로 나뉜다. 기존 모드의 새 요구사항 적용 유예는 2027-01-02까지다. 권한 이름은 "AI Training/Tool Usage"다. 런타임에 외부 AI를 호출하는 모드에 대한 조항은 없다. | help.nexusmods.com/article/180 |
| Chronicle ChatBox | Nexus 194491, kezc 제작, v1.2.30, GPL-3.0이다. Prisma UI 기반이다. 자막, 알림, 전투, 레벨업, 힘의 단어를 캐릭터별 기록으로 남기고 검색과 내보내기를 지원한다. 다른 플러그인이 구독할 수 있는 API(`ChronicleApi::Deliver`)가 있다. | Nexus API, GitHub kezc7/Chronicle |

## 3단계(자동 생성) 공급자별 추가 조사

| 공급자 | 결론 | 근거 |
|---|---|---|
| OpenAI | **공식 경로가 있다.** SIWC "ChatGPT plan usage"는 오픈소스·로컬 실행 앱이 사용자의 ChatGPT 플랜으로 Responses API를 호출하게 해 준다. OAuth PKCE와 127.0.0.1 루프백을 쓰고, client secret이 필요 없으며, `dynamic_agent_client`로 동적 등록한다. Plus는 5시간 한도를 다른 앱과 공유하고, Pro에는 5시간 한도가 없다. 아직 preview다. "open-source partners"라는 표현이 별도 승인이 필요하다는 뜻인지는 불명확하다. 약관 페이지는 403이라 읽지 못했다. | developers.openai.com/siwc/token-sharing-open-source, /siwc/quickstart |
| Claude | **회색지대다.** 사용자가 수정하지 않은 바이너리에 자기 구독으로 로그인하는 것은 허용한다. 서드파티가 사용자를 대신해 플랜 자격으로 요청을 중개하는 것은 금지한다. Consumer Terms §3.7은 API 키나 명시적 허가가 없는 "스크립트 등 자동화 접근"을 금지한다. 문서는 이 설계를 금지하지도 허용하지도 않는다. | code.claude.com legal-and-compliance, headless, authentication; anthropic.com consumer-terms |
| Gemini (Antigravity) | **자동화하면 안 된다.** 약관 6조는 "제3자 소프트웨어로 서비스에 접근하는 것은 계약 위반이며 계정 정지 사유"라고 명시한다. headless 모드(`agy -p`) 자체는 있다. | antigravity.google/terms, /docs/cli/headless |

## 확인하지 못한 것

- OpenAI SIWC 약관: 페이지가 403을 반환해 열지 못했다.
- Codex 보고서의 구독 한도 숫자표: 확인하지 않았으므로 인용하지 않는다.
- 서비스별 성인 창작물 정책의 세부 문구: 직접 읽지 않았다. 다만 세 회사 모두 노골적 성적 묘사를 제한한다는 방향은 기존에 알던 내용과 일치한다.
- 대화 훅 주소(DialogueHistory 기반), Prisma의 한글 IME와 긴 붙여넣기, SMF 3.8의 기능 범위, 실제 게임에서 이벤트가 발화하는지와 성능: 모두 실행 검증이 필요하다.
