현재 v0.1은 **“전체 활동을 빠짐없이 기록할 수 있는 명세”로 확정하면 안 됩니다.** 이벤트 이름 대부분은 존재하지만, 그 이벤트가 제공하지 않는 정보까지 기록할 수 있다고 적은 항목이 여럿 있습니다. 특히 **피해 훅의 의미·공존, 범죄 통계 발화 범위, 적중과 피해의 연결, 원시 활동을 버리는 묶음 정책**은 구현 전에 수정해야 합니다.

파일과 공개 소스만 읽었습니다. 파일 생성·수정·삭제, 설치·빌드·게임 실행, 바이너리 디컴파일은 하지 않았습니다. 아래의 **확인**은 문서·헤더·소스를 직접 확인했다는 뜻이며, 게임에서 동작을 재현했다는 뜻은 아닙니다.

근거 경로는 다음처럼 줄였습니다.

- **C**: [docs/activity-catalog.md](<G:/skyrim project/스카이림 이야기 생성기/docs/activity-catalog.md>)
- **D**: [docs/design-draft.md](<G:/skyrim project/스카이림 이야기 생성기/docs/design-draft.md>)
- **H**: `G:\skyrim project\스카이림 락픽 모드\external\CommonLibSSE-NG\include\RE\`
- **S**: `G:\TAKEALOOK\mods\Papyrus Compiler\source\scripts\`
- **M**: `G:\TAKEALOOK\mods\`
- **P**: [활성 프로필 modlist.txt](<G:/TAKEALOOK/profiles/TKL - MUNG ADDON - SPS Test/modlist.txt>)

심각도는 **높음: 핵심 기록 누락·오판·호환성 문제**, **중간: 특정 활동의 누락·부정확한 해석**, **낮음: 명세 불일치·표현 문제**로 구분했습니다.

**1. 피해·전투 기록에서 먼저 고쳐야 할 문제**

**① [높음] `Actor` vtable 하나를 훅하는 것으로는 수집 범위를 보장할 수 없습니다.**  
위치: C §2:50, D §4.3:194.

- **확인:** 함수 선언은 `Actor::HandleHealthDamage(Actor*, float)`입니다. `Character`는 `Actor`, `PlayerCharacter`는 `Character`를 상속하며, 세 클래스의 vtable 심볼이 따로 있습니다. 공개 TrueHUD 구현은 **`VTABLE_Actor[0]`, `VTABLE_Character[0]`, `VTABLE_PlayerCharacter[0]` 세 곳**에 각각 훅을 설치하고 원본 포인터도 따로 보관합니다.
- **판단:** 최소한 일반 NPC를 위한 `Character`, 플레이어 피격을 위한 `PlayerCharacter` 경로를 명시해야 합니다. 기반 `Actor`까지 포함할지는 실제 디스패치 범위를 확인해야 합니다. 기반 클래스 슬롯을 바꾸면 파생 클래스 슬롯도 자동 변경된다고 가정하면 안 됩니다.
- **런타임:** 로컬 래퍼는 비VR **0x104**, VR **0x106**을 사용합니다. 여기서 0x106은 AE용 인덱스가 아닙니다. SE/AE의 vtable 주소는 런타임별 relocation으로 구해야 합니다.
- **근거:** `H/A/Actor.h:475`, `H/C/Character.h:7`, `H/P/PlayerCharacter.h:285`, `H/Offsets_VTABLE.h:2142,2215,2231`; CommonLib `src/RE/A/Actor.cpp:2209`; [TrueHUD 설치 코드](https://github.com/ersh1/TrueHUD/blob/master/src/Hooks.h).

**② [높음] 실제 모드팩에 “설치 순서에 민감한 같은 함수 훅”이 있습니다.**  
위치: C §2:50, D §4.3:194.

- **확인:** 활성 `CalamityAffixes_MO2_v2.3.1_2026`의 공개 **v2.3.1 태그**는 위 세 vtable을 훅합니다. 그런데 기존 슬롯이 Skyrim 실행 파일 영역 밖을 가리키면 **다른 훅이 설치되었다고 판단하여 자기 훅 설치를 건너뜁니다.**
- **판단:** Novelist가 먼저 설치되면 Calamity의 피해 처리 경로가 비활성화될 가능성이 있습니다. “원본 호출 체인만 지키면 공존한다”로는 부족합니다.
- **공존 원칙:** 슬롯마다 **설치 직전에 들어 있던 함수**를 저장하고 정확히 한 번 호출해야 합니다. 하드코딩한 바닐라 함수를 호출하거나, 훅 안에서 같은 가상함수를 다시 호출하면 체인 우회·재귀 위험이 있습니다. 설치 순서, 후발 덮어쓰기, 중첩 콜백도 검증 대상입니다.
- **근거:** `P:29`, `M/CalamityAffixes_MO2_v2.3.1_2026/Docs/README.md:19`; [Calamity Hooks.cpp:127,253,268,296](https://github.com/servaltullius/calamity-reactive-loot-affixes/blob/v2.3.1/skse/CalamityAffixes/src/Hooks.cpp).

**③ [높음] 훅 인자 `damage`를 곧바로 “실제 깎인 체력”으로 저장하면 안 됩니다.**  
위치: C §2:50–58, D:194–206,246,536.

- **확인:** TrueHUD는 표시할 때 `-a_damage`를 사용합니다. Calamity는 인자를 조정해 다음 함수로 전달하고, 별도 지연 효과도 실행합니다. 따라서 훅 체인의 어느 위치에서 관측하느냐에 따라 값이 달라질 수 있습니다.
- **확인:** 헤더는 피해 적용 시점을 보장하지 않습니다. 공개 follower-framework는 이 함수를 *체력이 이미 감소한 뒤의 통지*라고 설명하는 반면, Calamity 코드는 원본 호출을 피해 적용 경계로 취급합니다. **공개 구현의 설명도 일치하지 않습니다.**
- **판단:** `reportedDamage`, 관측한 `healthBefore/After`, 실제 감소량으로 검증된 값, 추정치를 구분해야 합니다. “훅 진입 전 체력=맞기 전 체력”은 현재 근거로 확정할 수 없습니다.
- **추가 오류:** D:246의 “체력 비율은 훅이 이미 받은 값으로 계산하며 추가 조회가 없다”는 설명은 틀립니다. 인자에는 현재·최대 체력이 없습니다.
- **근거:** `H/A/Actor.h:475,557`; [TrueHUD 피해 처리](https://github.com/ersh1/TrueHUD/blob/master/src/Hooks.cpp), [Calamity 피해 전달:414–422](https://github.com/servaltullius/calamity-reactive-loot-affixes/blob/v2.3.1/skse/CalamityAffixes/src/Hooks.cpp), [피해 적용 후 통지라는 공개 구현 설명](https://github.com/Joao-Serrao/skyrim-follower-framework/blob/main/plugin/src/plugin.cpp).

**④ [높음] `TESHitEvent`와 피해 훅은 일대일 대응하지 않습니다.**  
위치: C §2:49–57, D:193–205.

- **확인:** `TESHitEvent`에는 공격 실행 ID·피해량·`HitData*`가 없습니다. `CriticalHit::Event`에는 피해 대상도 없습니다. CK 문서는 마법 효과가 붙은 공격에서 `OnHit`이 여러 번 발생하는 경우를 설명합니다.
- **확인:** Calamity v2.3.1은 추가타가 **이전 공격의 `lastHitData`를 가진 채** 피해 콜백에 들어오는 경우를 따로 처리합니다. 오래된 HitData로 추가 피해를 새 적중으로 오인하지 않도록 방어 코드도 있습니다.
- **판단:** “같은 대상의 가장 최근 적중”만 연결하면 부여마법·독·지속 피해·반사·추가타·다중 타격을 잘못 묶습니다. 단순 시간창 중복 제거는 빠른 실제 연타를 지울 수도 있습니다.
- **제안:** 공격 시도, 물리 충돌, 효과 적용, 피해 변경, 사망을 별개 관측으로 보관하고 연결 신뢰도를 붙이십시오. 연결되지 않은 피해도 보존해야 합니다.
- **근거:** `H/T/TESHitEvent.h:26`, `H/C/CriticalHit.h:17`, `H/M/MiddleHighProcessData.h:159`; [OnHit 문서](https://ck.uesp.net/wiki/OnHit_-_ObjectReference), [Calamity 추가타·오래된 HitData 처리](https://github.com/servaltullius/calamity-reactive-loot-affixes/blob/v2.3.1/skse/CalamityAffixes/src/Hooks.cpp).

**⑤ [높음] 함정·낙하 원인 분류가 잘못되었습니다.**  
위치: C §1:37, §2:62.

- **확인:** 피해 훅의 공격자는 **`Actor*`**입니다. 함정이나 돌 같은 일반 `TESObjectREFR`를 그 인자로 받는 구조가 아닙니다.
- **판단:** 공격자 없음은 낙하의 증거가 아닙니다. 소유자 없는 피해·환경 피해·일부 효과도 후보입니다. 반대로 함정 피해가 항상 공격자 없음으로만 관측된다는 보장도 없습니다.
- **제안:** 기본값은 `environmentalOrUnknown`. 낙하 경로·충돌·함정 활성화 등의 별도 근거가 있을 때만 세분화하십시오.
- **주의:** 로컬 `TESTrapHitEvent`는 전방 선언을 확인했지만 완전한 payload 정의는 찾지 못했습니다. 즉시 사용 가능한 완성 소스로 적으면 안 됩니다.
- **근거:** `H/A/Actor.h:475`, `H/S/ScriptEventSourceHolder.h:57`; [Acheron의 별도 낙하·물리 피해 훅](https://github.com/Scrabx3/Acheron/blob/main/src/Acheron/Hooks/Hooks.cpp).

**⑥ [높음] `percentBlocked`와 `stagger`는 실제 결과를 보장하지 않습니다.**  
위치: C §2:53,55, D:197,199.

- **확인:** 두 값은 `HitData` 필드이지 피해 훅 인자가 아닙니다. Maxsu Poise는 자체 경직 처리를 한 다음 `hitData->stagger = 0`으로 바꿉니다.
- **판단:** 현재 방식은 활성 모드팩에서 실제 경직을 놓칠 수 있습니다. `percentBlocked`도 읽은 HitData가 이번 피해의 것인지, 최종 방어 결과인지 먼저 확인해야 합니다.
- **제안:** `blockFlag`, 계산된 차단 비율, 실제 경직 시작·종료, 모드 포이즈 붕괴를 분리하십시오. 비율의 범위·분모도 실측 전에는 확정하지 마십시오.
- **근거:** `H/H/HitData.h:79,82`, `P:2204–2206`; [Maxsu Poise HitEventHandler.cpp:6–11](https://github.com/max-su-2019/MaxsuPoise/blob/master/src/Hooks/HitEventHandler.cpp).

**⑦ [높음] 결정타·처형·overkill을 같은 방식으로 확정할 수 없습니다.**  
위치: C §2:57, D:205,536.

- **확인:** `TESDeathEvent`는 죽은 개체·살해자·`dead`를 제공하지만 무기·피해량·처형 원인은 제공하지 않습니다. `Actor::IsInKillMove()`는 존재합니다.
- **판단:** 사망 시점의 마지막 피해가 반드시 사망 원인은 아닙니다. 지연 사망, 직접 Kill, 지속 피해, 처형 처리, 패배 모드가 개입합니다. 사망 이벤트 때 킬무브 플래그가 이미 내려갔다면 놓칩니다.
- **제안:** 킬무브 시작·종료 구간을 따로 기록하고 사망과 연결하십시오. `overkill`은 피해 적용 경계와 사전 체력이 검증된 경우에만 산출해야 합니다.
- **근거:** `H/T/TESDeathEvent.h:13`, `H/A/Actor.h:646`; `P:959`의 Acheron, [Acheron 피해 처리](https://github.com/Scrabx3/Acheron/blob/main/src/Acheron/Hooks/Hooks.cpp).

**⑧ [높음] “공격 전체”를 기록하는 소스가 빠져 있습니다.**  
위치: C §2:49,56, D:198,201,539.

- **확인:** 목록의 중심은 **적중**입니다. 근접 공격 시도·취소·헛손질의 독립 소스가 없습니다. 로컬 `SKSE/Events.h:42–52`에는 `kWeaponSwing`, `kSpellCast`, `kSpellFire`, `kBowDraw`, `kBowRelease`, 무기 뽑기·넣기 등이 있습니다.
- **판단:** 공격 시도가 없으면 빗나감 비율이나 “회피 중 상대가 공격했다”의 근거도 부족합니다. 활 이벤트와 적중이 연결되지 않았다는 이유만으로 빗나감이라고 하면, 비행 중·가로막힘·관측 누락도 빗나감으로 바뀝니다.
- **제안:** `attempt`, `release`, `cancel`, `impact`, `unresolved`를 구분하고, 마지막 상태를 자동으로 `miss`로 바꾸지 마십시오.

**2. 나머지 소스·의미 검증 결과**

아래에는 경미한 문제와 조건부 사용 가능한 항목도 포함했습니다.

| 심각도·위치 | 직접 확인한 사실 | 문제·제안 |
|---|---|---|
| **높음 C:13,60,106,113 / D:125** | `묶음`은 개별 기록을 버리고 합계만 남깁니다. detail 초과분도 카운터로 접습니다. | 사용자 요구인 “모든 활동을 세세하게”와 충돌합니다. **수집·보관·AI 전달 압축을 분리**해야 합니다. 영구적으로 버릴 범위를 성능 최적화로 숨기면 안 됩니다. |
| **중간 C:30** | `TESActorLocationChangeEvent`는 `BGSLocation` 변경입니다. 한 Location에 여러 실내·외 셀이 들어갈 수 있습니다. | 건물 출입 전체를 보장하지 않습니다. 셀·월드스페이스·플레이어 위치 전환을 보완해야 합니다. 근거: `H/T/TESActorLocationChangeEvent.h:14`; [OnLocationChange](https://ck.uesp.net/wiki/OnLocationChange_-_Actor). |
| **중간 C:31 / D:151** | `LocationDiscovery::Event`에는 `MapMarkerData*`, `const char* worldspaceID`만 있습니다. | 마커 참조 키가 payload에 있다는 식으로 구현할 수 없습니다. 문자열 worldspace 식별과 실제 폼 ID도 구분해야 합니다. `H/L/LocationDiscovery.h:16`. |
| **중간 C:32** | `LocationCleared::Event`는 비어 있습니다. | “현재 장소”가 실제 클리어 장소라는 보장은 없습니다. 추정 표시는 옳지만, 원인 장소 미상도 허용해야 합니다. `H/L/LocationCleared.h:12`. |
| **중간 C:33–34** | 빠른 이동 이벤트에는 시간 값만 있고 출발·도착·수단이 없습니다. | 지도 열기 취소, 같은 Location 내 이동, 지도 없는 이동을 처리해야 합니다. 다음 Location 이벤트가 반드시 도착 이벤트는 아닙니다. “며칠”도 실제 경과 시간에 맞춰야 합니다. `H/T/TESFastTravelEndEvent.h:9`. |
| **중간 C:38** | CK의 MiscStat 조회 목록에는 이동 거리 항목이 없습니다. | “거리는 게임 통계로 충분”은 근거가 없습니다. 거리 필요 시 위치 샘플 기반 추정과 순간이동 제외 규칙이 필요합니다. [QueryStat](https://ck.uesp.net/wiki/QueryStat_-_Game). |
| **높음 C:48 / D:154** | 전투 이벤트는 개체의 상태 전이입니다. 전투 전체의 시작·끝 이벤트가 아닙니다. | 추적 중 NPC가 죽거나 언로드되어 `none`을 못 받는 경우, 비전투 중 첫 공격, 패배·도주·수색 종료를 보완해야 합니다. `H/T/TESCombatEvent.h:20`. |
| **중간 C:59,174 / D:533** | `TESEnterBleedoutEvent`는 진입만 제공합니다. | 쓰러졌다가 일어나는 사건이 없습니다. 회복 전이 폴링·모드 구조 이벤트를 추가해야 합니다. `H/T/TESEnterBleedoutEvent.h:13`, `H/A/ActorState.h:163`. |
| **중간 C:68–69** | 시전 이벤트는 시전자와 FormID만 제공합니다. CK 문서는 집중 주문에서 시작 때 한 번 발생한다고 설명합니다. | 시전 시간·중단·유지·소환 성공을 알 수 없습니다. FormID가 항상 일반 `SpellItem`이라고 가정해서도 안 됩니다. `H/T/TESSpellCastEvent.h:13`; [OnSpellCast](https://ck.uesp.net/wiki/OnSpellCast_-_ObjectReference). |
| **높음 C:70,82** | 효과 적용 이벤트는 효과를 적용하려는 시점의 신호입니다. 적용/제거 이벤트에는 효과 폼 대신 `activeEffectUniqueID`가 있습니다. | 적용 시도와 실제 활성 효과를 구분해야 합니다. 질병 치료는 같은 질병 효과의 남은 인스턴스까지 확인해야 하며, 제거 이벤트 때 이미 사라진 효과를 조회하는 방식은 실패할 수 있습니다. `H/T/TESActiveEffectApplyRemoveEvent.h:15`; [OnMagicEffectApply](https://ck.uesp.net/wiki/OnMagicEffectApply_-_ObjectReference). |
| **중간 C:71 / D:204** | `ShoutAttack::Event`에는 `TESShout*` 하나만 있습니다. | 사용 단어 수와 NPC 시전자를 직접 얻을 수 없습니다. 별도 캐스팅 상태가 필요합니다. `H/S/ShoutAttack.h:16`. |
| **중간 C:72** | `GetKnown()`은 `TESWordOfPower`의 부모 `TESForm`에 존재합니다. | API 자체는 유효합니다. 다만 **단어 발견·학습과 영혼을 써서 해금**은 별도 활동입니다. `H/T/TESForm.h:148`, `H/T/TESWordOfPower.h:10`. |
| **중간 C:73 / D:159** | 드래곤 영혼 이벤트에는 `souls`만 있고 드래곤 참조가 없습니다. | 직전 드래곤 사망과 자동 연결하면 다른 흡수·보상·모드 부여를 오인할 수 있습니다. 값이 증분인지 등도 확인해야 합니다. `H/D/DragonSoulsGained.h:14`. |
| **중간 C:84** | 종족 전환 완료 이벤트에는 `subject`만 있습니다. | 이전·이후 종족 캐시가 필요합니다. 이 이벤트만으로 영구 흡혈귀화와 일시 변신을 구분할 수 없습니다. `H/T/TESSwitchRaceCompleteEvent.h:13`. |
| **중간 C:85–86** | 활성화 이벤트는 대상과 실행자를 제공합니다. | 수호석을 눌렀다가 선택을 취소해도 “선택”으로 기록할 위험이 있습니다. 능력·효과 변경으로 확정해야 합니다. 제단도 활성화와 효과 적용을 연결해야 합니다. `H/T/TESActivateEvent.h:13`, `S/TempleBlessingScript.psc:5–10`. |
| **중간 C:97** | `SkillIncrease::Event`는 스킬 ID를 주지만 상승 전후 수치는 주지 않습니다. | 상승량·100 도달 판정에는 값의 기준선이 필요합니다. Uncapper·정적 성장 모드가 있는 환경에서는 100도 일률적인 종착점이 아닙니다. `H/S/SkillIncrease.h:17`, `P:2171,2173`. |
| **중간 C:105,113,120–121** | Container/Equip 이벤트에는 base form과 제한적인 인스턴스 식별자가 있습니다. 인벤토리에는 별도 extra data가 있습니다. | 같은 base form의 강화·부여마법·개명 장비를 합치면 고유 장비의 이력이 사라집니다. 퀘스트 물품 여부도 base form만으로 끝낼 수 없습니다. `H/T/TESContainerChangedEvent.h:11`, `H/I/InventoryEntryData.h:40,57,66`. |
| **중간 C:107** | `ChestsLooted::Event`는 빈 payload입니다. | 어느 상자·시체를 뒤졌는지, 열기만 했는지, 어떤 물품을 얻었는지 제공하지 않습니다. `H/C/ChestsLooted.h:10`. |
| **높음 C:108–109,176** | 컨테이너 변화에는 이동 이유가 없습니다. | NPC로 이동했다고 선물·퀘스트 전달, 목적지가 0이라고 버림으로 확정할 수 없습니다. 판매·보관·소모·삭제·스크립트 회수와 구분해야 합니다. `H/T/TESContainerChangedEvent.h:11`. |
| **중간 C:120–122** | 제작 이벤트 플래그는 로컬 헤더에서도 `unk08/09/0A`이고 주석으로만 의미가 설명됩니다. 수량·레시피·작업대·강화 전후 수치는 없습니다. | 제작 종류 판정을 완성된 enum처럼 쓰지 마십시오. 요리·제련·무두질·강화·인챈트의 매핑을 별도 검증해야 합니다. `H/I/ItemCrafted.h:16–19`. |
| **중간 C:125** | 수면·대기 종료 이벤트는 `interrupted`만 제공합니다. | 시작 시점의 시간·장소·침대·수면/대기 종류를 보관해야 합니다. 취소·중단·도중 이동도 처리해야 합니다. `H/T/TESSleepStopEvent.h:9`, `H/T/TESWaitStopEvent.h:9`, `H/S/SleepWaitMenu.h:26`. |
| **중간 C:127–128** | Furniture 이벤트는 입장·퇴장입니다. 장작 스크립트는 `AddToInventory` 애니메이션 이벤트에서 생산물을 지급합니다. 수확 전용 이벤트도 있습니다. | 가구 이용 횟수는 채광·장작 생산 횟수가 아닙니다. 생산물 이동과 결합하고 `TESHarvestedEvent::ItemHarvested`도 사용하십시오. `S/WoodChoppingScript.psc:55–70`, `S/MineOreFurnitureScript.psc:185`, `H/I/ItemHarvested.h:16`. |
| **중간 C:131 / D:158** | `BooksRead`는 존재하고 책·skillBook을 줍니다. 별도로 `TESBookReadEvent`도 있습니다. | 통계용 BooksRead가 재독까지 모두 알린다고 헤더만으로 보장할 수 없습니다. 재독·문서 열기 검증에 BookMenu와 TESBookReadEvent를 대조해야 합니다. “열었다”와 “내용을 읽었다”도 구분하십시오. `H/B/BooksRead.h:16`, `H/T/TESBookReadEvent.h:13`. |
| **중간 C:153** | 잠금 변경 이벤트는 잠금·해제·등급 변경에 쓰입니다. | 열쇠·주문·스크립트·Lock Bashing도 자물쇠 따기로 오인할 수 있습니다. `LocksPicked::Event`와 LockpickingMenu 문맥을 결합하십시오. 실패한 락픽 수는 payload에 없습니다. `H/T/TESLockChangedEvent.h:13`, `H/L/LocksPicked.h:15`, `P:2471`; [잠금 변경 문서](https://ck.uesp.net/wiki/OnLockStateChanged_-_ObjectReference). |
| **중간 C:154** | `ItemsPickpocketed::Event`에는 수량만 있습니다. | 대상·물품·성공한 시도 횟수·실패·역소매치기는 별도 근거가 필요합니다. `H/I/ItemsPickpocketed.h:14`. |
| **높음 C:157** | 제안된 소스에는 목격자 여부를 입증하는 정보가 없습니다. | 범죄 직후 죽은 사람을 “목격자 처리”로 쓰는 것은 추정 표시만으로도 너무 강합니다. 관측된 목격자 집합 또는 명확한 게임 통지가 없으면 **범죄 뒤 발생한 사망**으로만 보관하십시오. `H/T/TESTrackedStatsEvent.h:11`, `H/T/TESDeathEvent.h:13`. |
| **중간 C:163,168** | 저널에 표시된 퀘스트만 남기고 숨은 퀘스트를 제외합니다. | 숨은 퀘스트에는 관리 코드뿐 아니라 결혼·가족·생활 상태를 운영하는 시스템도 있습니다. “서사로 쓰지 않음”과 “활동 탐지 소스로도 쓰지 않음”을 분리해야 합니다. `S/BYOHRelationshipAdoptionPlayerScript.psc`, C:142와도 범위가 충돌합니다. |
| **중간 C:165** | `Actor::GetFactionRank()`와 `IsInFaction()`이 존재합니다. | “그 밖의 근거는 없다”는 설명은 틀립니다. 추적 대상 팩션의 등급 비교가 가능합니다. 다만 멤버십 변화가 자발적 가입이라는 뜻은 아닙니다. `H/A/Actor.h:464,580`. |
| **중간 C:177** | `GetMount`, `QLastRiddenMount`가 있습니다. | 최근 탄 말과 소유한 말은 다릅니다. 동료·반려동물·소환수·훔친 말의 역할을 분리해야 합니다. `H/A/Actor.h:490,590,633`. |
| **중간 C:190–192** | Sky에는 현재·이전 날씨와 혼합 비율, Calendar에는 시간이 있습니다. | 장소·전투 시작 때만 읽으면 같은 장소의 밤낮·폭풍 전환을 놓칩니다. 이름 있음과 `IsUnique`도 동의어가 아닙니다. 이 모드팩에는 NPC 이름 배포 모드가 있습니다. `H/S/Sky.h:105,133`, `H/C/Calendar.h:72`, `P:2262`. |
| **낮음 C:5,55,138 / D:121,175** | 헤더의 정확한 이름은 `DisarmedEvent::Event`, 대화 필드는 `topicInfoFormID`입니다. D는 큐를 세 등급이라고 쓰지만 detail까지 네 종류입니다. | 헤더 확인·실행 확인을 별도 열로 나누고 이름과 사건 등급 스키마를 통일하십시오. |

**3. 소모품·대화·거래·콘솔의 구체적인 결론**

**소모품 — C:123–124, D:213**

- **포션:** CK의 `OnObjectEquipped` 문서는 포션 같은 소모품도 포함한다고 명시합니다. 따라서 “포션도 equip 계열 경로를 쓴다”는 근거는 있습니다.
- **음식:** 더 직접적인 C++ 사례가 있습니다. 활성 모드인 Survival Mode Improved의 `OnEquipEventHandler`가 `TESEquipEvent`에서 `AlchemyItem`을 찾고 `IsFood()`를 검사해 허기를 처리합니다. 로컬 생존 스크립트도 `OnObjectEquipped`를 사용합니다.
- **독:** 이번 조사에서는 **독 바르기의 확정·취소·실제 적용까지 TESEquipEvent 하나로 정확하게 판별한다는 직접 근거를 확보하지 못했습니다.** 포션 근거를 독까지 확대하면 안 됩니다. `PoisonedWeapon::Event` 형식은 있지만 로컬 헤더에는 이벤트 소스 접근자가 없습니다.
- **추가 함정:** 이 모드팩에는 UAPNG가 있습니다. 공개 소스는 equip 경로를 가로채 애니메이션과 실제 소비 시점을 조정합니다. 사용 요청 때 체력과 실제 삼켰을 때 체력은 다를 수 있습니다.
- **제안:** `useRequested`, `consumed`, `effectApplied`, `cancelled`를 가능한 범위에서 구분하고, 사용 전 체력이라는 표현은 시점 검증 후 사용하십시오.

근거: [OnObjectEquipped](https://ck.uesp.net/wiki/OnObjectEquipped_-_Actor), [SMI Events.h:283–300](https://github.com/colinswrath/Survival-Mode-Improved-SKSE/blob/main/include/Events.h), `S/Survival_HungerEatingDetection.psc:20`, `H/P/PoisonedWeapon.h:8`, [UAPNG equip 훅](https://github.com/QY-MODS/SKSE_UAPNG/blob/main/src/equip_object_override.cpp).

**대화 — C:137–140**

`TESTopicInfoEvent`는 **NPC가 어떤 INFO를 시작·종료했는지 추적하는 소스로 쓸 수 있습니다.** 그러나 실제 재생된 최종 문장을 직접 주지는 않습니다.

- **확인:** payload는 화자, `topicInfoFormID`, 시작/종료 종류, stage입니다. 청자나 자막 문자열은 없습니다.
- **판단:** INFO의 응답 목록을 전부 기록하면 실제로 재생하지 않은 응답까지 수집할 수 있습니다. 치환 문자열, 여러 응답, 중간 끊김도 고려해야 합니다.
- **자막 폴링:** 실제 화면 문장을 얻는 장점이 있지만 짧은 자막, 빠른 넘김, 자막 설정, 겹치는 대사, 동일 문장 반복에서 누락·중복 위험이 있습니다.
- **플레이어 선택:** `MenuTopicManager::lastSelectedDialogue`와 선택 상태는 존재하지만 폴링 사이의 빠른 선택을 모두 보장하지 않습니다.
- **권고:** **INFO 이벤트로 화자·퀘스트·대화 문맥을 잡고, 자막/응답 재생 경로로 실제 문장을 보충**하십시오. “플레이어에게 한 말”은 DialogueMenu가 열렸다는 조건만으로 판정하지 마십시오. 강제 인사·장면 대사·동료 발언은 메뉴 밖에서도 발생할 수 있습니다.

근거: `H/T/TESTopicInfoEvent.h:25`, `H/M/MenuTopicManager.h:74–81`, `H/S/SubtitleManager.h:13–15`. 공개 [Chronicle Events.cpp](https://github.com/kezc7/Chronicle/blob/main/src/Events.cpp)는 INFO 추적과 자막 폴링을 함께 사용하고, [Events.h:268](https://github.com/kezc7/Chronicle/blob/main/src/Events.h)의 폴링 간격은 100ms입니다. 이것은 구현 사례이지 무손실 보장은 아닙니다.

**거래 — [높음] C:110–111, D:233**

- **확인:** 컨테이너 변화에는 거래 가격이나 거래 실행 ID가 없습니다. `BarterMenu::GetTargetRefHandle()`과 `IsViewingVendorItems()`는 존재합니다.
- **판단:** 메뉴 시작·종료 시 순변화만 비교하면 **샀다가 되팔기, 동일 base form의 매수·매도, 묶음 거래, 돈 없는 상인에게 판매**를 복원할 수 없습니다.
- **판단:** 물품이 상인 본인의 인벤토리 대신 거래용 컨테이너로 이동할 수 있습니다. 메뉴가 열려 있는 동안 다른 스크립트가 물품·금화를 변경하는 경우도 거래로 오염됩니다.
- **제안:** 거래 세션 안의 **개별 전송 순서**와 상인·거래 컨테이너를 보관하십시오. 정확한 개별 가격이 필요하면 거래 확정 경로가 필요합니다. 연결 실패 시에는 “거래 중 인벤토리 변화, 개별 가격 미상”으로 기록하십시오.
- **추가:** 금화는 ContainerChanged로 증감을 받을 수 있으므로 매번 인벤토리를 재계산하는 폴링보다 이벤트+낮은 빈도 대조가 낫습니다.

근거: `H/T/TESContainerChangedEvent.h:11–16`, `H/B/BarterMenu.h:59–60`.

**콘솔·치트 — [높음] C:199–200,225**

- **확인:** `Console::MENU_NAME == "Console"`이며 `UI::IsMenuOpen()`으로 검사할 수 있습니다. 메뉴 열림/닫힘 이벤트와 함께 사용할 수 있습니다.
- **확인:** `PlayerCharacter::IsGodMode()`가 존재하고 로컬 구현은 엔진의 bool 값을 읽습니다.
- **미확인:** 이번 로컬 API 조사에서는 TCL·TIM 등을 모두 포괄하는 통합 “치트 상태” 조회를 확인하지 못했습니다.
- **판단:** 콘솔 열림과 사건의 원인은 다릅니다. 닫은 뒤 실행되는 스크립트, 외부 ConsoleUtil 호출, MCM 물품 지급은 열림 검사로 못 잡습니다. 반대로 콘솔이 열린 중의 정상 사건을 모두 치트로 분류할 수 있습니다.
- **제안:** `consoleOpenAtObservation`, `godModeAtObservation`, `originKnown/Unknown`을 따로 저장하십시오. 콘솔 열림 때 오염된 인벤토리·통계 기준선도 갱신해야 닫은 직후 정상 획득으로 다시 기록하지 않습니다. **모든 치트 사건을 완전히 식별한다는 약속은 제거해야 합니다.**

근거: `H/C/Console.h:22`, `H/U/UI.h:92`, `H/P/PlayerCharacter.h:726`; CommonLib `src/RE/P/PlayerCharacter.cpp:27–30`.

**4. 폴링 항목의 실제 API와 한계**

아래의 주기는 **구현 출발점 제안**이며 성능 측정 결과가 아닙니다. 폴링은 값이 바뀌었을 때 사건을 만들고, 매 샘플을 사건으로 저장하지 않는 것을 전제로 합니다.

| 항목·위치 | 확인된 접근 경로 | 판정 한계·권장 주기 |
|---|---|---|
| 은신·탐지 C:150–151 | `Actor::IsSneaking()`, `ProcessLists::RequestHighestDetectionLevelAgainstActor(actor, LOSCount)`, 관찰자별 `RequestDetectionLevel(target, priority)` | 은신 눈 표시·탐지 강도·누가 탐지했는지는 별개입니다. 관찰자→플레이어 방향을 주의해야 합니다. 0.25–0.5초는 짧은 전이를 놓칠 수 있으므로 정확한 시각으로 쓰지 마십시오. 근거 `H/A/Actor.h:662,678`, `H/P/ProcessLists.h:48`. |
| 탑승 C:35 | `IsOnMount()`, `GetMount(out)`, `GetMountedBy(out)` | 말뿐 아니라 드래곤 등을 구분할 모델이 필요합니다. 0.25–0.5초+전환 신호. `H/A/Actor.h:590,653`. |
| 수영·잠수 C:36 | `ActorState::IsSwimming()`, `Actor::GetSubmergedLevel()`, 런타임 `underWaterTimer` | 수영과 머리 잠김은 다릅니다. 수중 호흡·모드 잠수도 고려해야 합니다. 0.25–0.5초. `H/A/ActorState.h:202`, `H/A/Actor.h:599,722`. |
| 레벨업 선택 C:95 | `ActorValueOwner::GetBaseActorValue()` 전후 비교 | **선택 결과 추정**은 가능하지만 메뉴 한 번에 여러 레벨을 처리하면 순서가 사라집니다. 스크립트 기본값 변경과도 혼동됩니다. 개별 선택 콜백 관측이 더 정확합니다. `H/A/ActorValueOwner.h:18`. |
| 퍽 C:96 | 플레이어 런타임 `addedPerks`, `perks`, `HasPerk()` | base NPC 목록만 보면 안 됩니다. rank, 퍽 체인, 환불·전설화, 모드의 메뉴 밖 지급을 처리해야 합니다. 메뉴 경계+알려진 변경 계기, 낮은 빈도 대조. `H/P/PlayerCharacter.h:832–834`, `H/A/Actor.h:617`. |
| 생존 욕구 C:87 | `TESGlobal::value`; 활성 모드에 맞는 폼 매핑 | 아래의 SMI 매핑을 사용 가능 후보로 확인했습니다. 1–5초+식사·수면 직후. 악화뿐 아니라 회복 전이도 필요합니다. `H/T/TESGlobal.h:45`. |
| 관할 현상금 C:156 | `TESFaction::GetCrimeGold()`, 폭력/비폭력별 getter; 플레이어 `crimeGoldMap` | 9개 영지만 고정하면 오크·DLC·모드 관할이 빠집니다. 0.5–1초+범죄·대화 경계. 현상금 감소만으로 벌금·수감·목격자 제거를 결정하지 마십시오. `H/T/TESFaction.h:158–160`, `H/P/PlayerCharacter.h:500`. |
| 킬무브 C:57 | `Actor::IsInKillMove()` | 저빈도 폴링이나 사망 때 한 번 조회로는 누락됩니다. 동작 시작·종료 관측이 우선입니다. `H/A/Actor.h:646`. |
| 동료 C:141 | `Actor::IsPlayerTeammate()` | 동료 명단 전체를 반환하는 API가 아닙니다. 알려진 동료·퀘스트 별칭·로드 상태를 결합해야 합니다. D의 10초×3회는 해산 판정을 수십 초 늦추고 짧은 합류를 놓칩니다. `H/A/Actor.h:657`, D:163. |
| 관계 등급 C:143 | Papyrus `Actor.GetRelationshipRank(other)`; C++ `BGSRelationship::GetRelationship(base1, base2)` | 로컬 Actor C++에 같은 이름의 직접 getter는 확인하지 못했습니다. BGSRelationship enum은 0=연인, 4=지인, 8=최대 적대이며 Papyrus 등급과 그대로 같지 않습니다. 기록 변화가 감정 변화의 원인까지 말해주지는 않습니다. `S/Actor.psc:284`, `H/B/BGSRelationship.h:15,62`. |

SMI 공개 소스는 다음을 사용합니다. **현재 설치 DLL과 공개 HEAD가 동일하다는 검증은 하지 않았으므로 버전 고정 후 확인해야 합니다.**

| 값 | 소스가 조회하는 파일·로컬 FormID |
|---|---|
| 활성화 | `ccqdrsse001-survivalmode.esl : 000826` |
| 허기 값 | 같은 파일 `00081A` |
| 피로 값 | 같은 파일 `000816` |
| 추위 값 | 같은 파일 `00081B` |
| SMI 허기 단계 | `SurvivalModeImproved.esp : 000A14` |
| SMI 피로 단계 | 같은 파일 `000A1C` |
| SMI 추위 단계 | 같은 파일 `000D1E` |

근거: [SMI FormLoader.h:14–15,86–87,174–175,259–260,307](https://github.com/colinswrath/Survival-Mode-Improved-SKSE/blob/main/include/Settings/FormLoader.h). 활성 프로필에는 SMI 외에 Starfrost도 있으므로 **바닐라 임계값을 고정하면 안 됩니다**. 근거: `P:2738,4685`, 프로필 `plugins.txt:255,2028–2030`.

**5. `TESTrackedStatsEvent`와 실제 MiscStat 이름**

**[높음] C:98,112,155–156의 통계 채널은 “조회 가능”과 “이벤트 발화”를 구분해야 합니다.**

- **확인:** `TESTrackedStatsEvent`의 데이터는 이름과 정수 값뿐입니다. CK의 `OnTrackedStatsEvent` 문서는 값이 **갱신된 누적값**이라고 설명하고, 모든 통계가 이 이벤트를 발생시키지는 않는다고 명시합니다.
- **확인:** 문서의 발화 목록에는 `Assaults`, `Murders`, `Items Stolen`, `Trespasses`, `Training Sessions`가 없습니다.
- **한계:** 문서 목록도 완전하다고 확정할 수 없습니다. 로컬 `AchievementsScript.psc:71–76`은 `Potions Mixed`, `Poisons Mixed`, `StalhrimItemsCrafted`도 처리하지만 해당 위키 발화 목록에는 없습니다.
- **결론:** 범죄·생활 통계를 전부 이벤트로 받는 설계는 성립하지 않습니다. **통계별 발화 여부 표 + 조회 대조**가 필요합니다. 통계 증가만으로 피해자·물품·원인·정확한 발생 시각까지 복원할 수는 없습니다.

근거: `H/T/TESTrackedStatsEvent.h:11–12`, `S/AchievementsScript.psc:43–81`; [OnTrackedStatsEvent 문서](https://ck.uesp.net/wiki/OnTrackedStatsEvent_-_Form).

CK `QueryStat` 문서에서 직접 확인한 **조회 이름 104개**는 다음과 같습니다. 명칭 목록이며, 모두 사건으로 발화한다는 뜻은 아닙니다. [QueryStat 원문](https://ck.uesp.net/wiki/QueryStat_-_Game)

| 구분 | 정확한 조회 이름 |
|---|---|
| 탐험·생활 | `Locations Discovered`, `Dungeons Cleared`, `Days Passed`, `Hours Slept`, `Hours Waiting`, `Standing Stones Found`, `Gold Found`, `Most Gold Carried`, `Chests Looted`, `Skill Increases`, `Skill Books Read`, `Food Eaten`, `Training Sessions`, `Books Read`, `Horses Owned`, `Houses Owned`, `Stores Invested In`, `Barters`, `Persuasions`, `Bribes`, `Intimidations` |
| 신체 변화 | `Diseases Contracted`, `Days as a Vampire`, `Days as a Werewolf`, `Necks Bitten`, `Vampirism Cures`, `Werewolf Transformations`, `Mauls` |
| 퀘스트 | `Quests Completed`, `Misc Objectives Completed`, `Main Quests Completed`, `Side Quests Completed`, `The Companions Quests Completed`, `College of Winterhold Quests Completed`, `Thieves' Guild Quests Completed`, `The Dark Brotherhood Quests Completed`, `Civil War Quests Completed`, `Daedric Quests Completed`, `Dawnguard Quests Completed`, `Dragonborn Quests Completed`, `Questlines Completed` |
| 전투 | `People Killed`, `Animals Killed`, `Creatures Killed`, `Undead Killed`, `Daedra Killed`, `Automatons Killed`, `Favorite Weapon`, `Critical Strikes`, `Sneak Attacks`, `Backstabs`, `Weapons Disarmed`, `Brawls Won`, `Bunnies Slaughtered` |
| 마법 | `Spells Learned`, `Favorite Spell`, `Favorite School`, `Dragon Souls Collected`, `Words Of Power Learned`, `Words Of Power Unlocked`, `Shouts Learned`, `Shouts Unlocked`, `Shouts Mastered`, `Times Shouted`, `Favorite Shout`, `Soul Gems Used`, `Souls Trapped` |
| 제작·소비·채집 | `Magic Items Made`, `Weapons Improved`, `Weapons Made`, `Armor Improved`, `Armor Made`, `Potions Mixed`, `Potions Used`, `Poisons Mixed`, `Poisons Used`, `Ingredients Harvested`, `Ingredients Eaten`, `Nirnroots Found`, `Wings Plucked` |
| 범죄 | `Total Lifetime Bounty`, `Largest Bounty`, `Locks Picked`, `Pockets Picked`, `Items Pickpocketed`, `Times Jailed`, `Days Jailed`, `Fines Paid`, `Jail Escapes`, `Items Stolen`, `Assaults`, `Murders`, `Horses Stolen`, `Trespasses` |
| 관할 | `Eastmarch Bounty`, `Falkreath Bounty`, `Haafingar Bounty`, `Hjaalmarch Bounty`, `The Pale Bounty`, `The Reach Bounty`, `The Rift Bounty`, `Tribal Orcs Bounty`, `Whiterun Bounty`, `Winterhold Bounty` |

추가로 로컬 스크립트에서 `NumWerewolfPerks`, `NumVampirePerks`, `StalhrimItemsCrafted`를 확인했습니다. 따라서 위 104개를 엔진의 완전한 내부 통계 사전이라고 부르면 안 됩니다. 근거: `S/DLC1_QF_CreatureDialogueWerew_00000E46.psc:10`, `S/DLC1_QF_DLC1DialogueVampire_0101964A.psc:10`, `S/AchievementsScript.psc:76`.

`Favorite …`는 횟수 증가 사건으로 처리할 대상이 아니며, `Most Gold Carried`, `Largest Bounty`는 최대값입니다. `Gold Found`는 현재 소지 금액이 아닙니다. 로드 시 값 감소·초기 기준선·콘솔 변경을 범죄 취소나 역방향 활동으로 해석하지 않아야 합니다.

**6. 활성 모드팩에서 확인한 전투 모드·태그**

설치 여부는 `modlist.txt`의 `+`와 해당 파일로 확인했습니다. **활성 모드 폴더에 있다는 사실과 실제 로드된 DLL이 그 파일이라는 사실은 다릅니다.** 같은 이름의 DLL을 제공하는 모드가 여럿 있었고, 실행 로그·실제 VFS 승자까지 확인한 결과는 아닙니다.

| 모드·활성 근거 | 확인한 태그·이벤트 또는 처리 | 기록기에서의 해석 |
|---|---|---|
| **TK Dodge RE / Addon** `P:2221–2223` | `TKDodgeForward/Back/Left/Right`는 시작 요청. 공개 RE 소스는 **`TKDR_DodgeStart`를 수신해 비용을 적용**합니다. 로컬 behavior 텍스트에는 `TKDodgeStop`도 있습니다. | 키 입력·시작 요청과 실제 동작 시작을 분리하십시오. Addon 공개 저장소 URL은 이번 조회에서 404였으므로 기반 RE와 동일하다고 확정할 수 없습니다. [RE AnimationEvents.cpp](https://github.com/max-su-2019/TK-Dodge-RE/blob/main/src/AnimationEvents.cpp); `M/TK Dodge RE Addon/skse/plugins/TK Dodge RE.ini:20–22`; RE의 `Nemesis_Engine/mod/tkds/_1stperson/magicbehavior/#0077.txt:101–108`. |
| **Ultimate NPC Dodging** `P:2219` | **`UND_DodgeEvent` ModCallbackEvent**, sender=회피 Actor, 빈 문자열, 숫자 0. `bIframeActive`, `bInIframe`도 설정합니다. | NPC 회피는 애니메이션 추정보다 직접 이벤트를 우선 검토할 수 있습니다. 단, 성공적으로 공격을 피했다는 뜻은 아닙니다. [dodge.cpp:851–870](https://github.com/LeoneKingzz/Ultimate-NPC-Dodging-PoiseBuild/blob/main/src/dodge.cpp). |
| **IFrame Generator RE / AE Support** `P:1228–1229` | `bIframeActive && bInIframe` 또는 애니메이션 annotation 기반 무적 구간. | 무적 상태는 태그 이름 하나가 아닙니다. 회피 동작·무적 구간·실제 적중 무효화를 따로 다뤄야 합니다. [Hooks.h](https://github.com/max-su-2019/MaxsuIFrame/blob/main/src/Hooks.h), [제작자 문서](https://github.com/max-su-2019/MaxsuIFrame/blob/main/doc/en/tutorial.md). |
| **Parry for All** `P:165` | 성공: `ParriedCMF`, `PerfParriedCMF`; 상대: `GotParriedCMF`, `GotPerfParriedCMF`. 로컬 BDI에서 네 이벤트 모두 **`notify: true`**. 공개 소스에는 `ParryStartCMF/EndCMF`, `PerfParryStartCMF/EndCMF` 같은 판정 창도 있습니다. | **성공 이벤트와 판정 창을 구분**할 수 있습니다. Precision 경로에서는 적중을 무시하므로 Hit 이벤트만 보면 성공 패링을 놓칠 수 있습니다. `M/Parry for All/SKSE/Plugins/BehaviorDataInjector/ParryForAll_BDI.json:5–24`; [Hooks.cpp](https://github.com/vinymayan/Parry-for-all/blob/main/src/Hooks.cpp), [Events.cpp](https://github.com/vinymayan/Parry-for-all/blob/main/src/Events.cpp). |
| **Maxsu Block Overhaul** `P:2203` | 로컬 behavior에 `Maxsu_BlockHitWinOpen`, `Maxsu_BlockHitWinClose`, `Maxsu_GuardBreakStart`, `Maxsu_GuardBreakStop`, 변수 `Maxsu_IsGuardBreak`. | 가드 브레이크 후보 신호입니다. 이벤트 사전에 존재하는 것과 외부 sink로 통지되는 것은 다르므로 발화 검증이 필요합니다. `M/Maxsu Block Overhaul/Nemesis_Engine/mod/block/1hm_behavior/#0085.txt:465–468,575`. |
| **Maxsu Poise** `P:2204–2205` | 자체 경직 처리 후 `HitData::stagger=0`. | 포이즈 피해·포이즈 붕괴·일반 체력 피해를 혼합하면 안 됩니다. [HitEventHandler.cpp](https://github.com/max-su-2019/MaxsuPoise/blob/master/src/Hooks/HitEventHandler.cpp). |
| **Modern Stagger Lock** `P:2206` | `MSL_SmallStaggerStart`, `MSL_MediumStaggerStart`, `MSL_LargeStaggerStart`, `MSL_LargestStaggerStart`, `MSL_SpecialStaggerStart`, `staggerStop`. | 실제 경직의 종류·종료 기록 후보입니다. 공개 소스와 설치 AE 변형의 일치 여부는 미확인입니다. [Hooks.cpp:121–175](https://github.com/max-su-2019/ModernStaggerLock/blob/main/src/Hooks.cpp). |
| **Dual Wield Parrying SKSE** `P:1247` | 공개 입력 처리에서 `blockStart`, `blockStop`을 보냅니다. | 이름이 Parrying이어도 이것만으로 타이밍 패링 성공이라고 해석하면 안 됩니다. [InputEventHandler.cpp:156,162](https://github.com/DennisSoemers/DualWieldParryingSKSE/blob/master/src/InputEventHandler.cpp). |
| **Precision** `P:2216` | HitData 생성·무기/강타/맨손 피해 계산 훅, API 콜백 경로. | 충돌·무시된 적중·수정된 피해를 구분할 때 API 활용을 검토하십시오. 일반 태그 채널만으로 끝나지 않습니다. [Hooks.h:54–73](https://github.com/ersh1/Precision/blob/master/src/Hooks.h). |
| **Acheron** `P:959` | 로컬 API에 `OnActorDefeated`, `OnActorRescued`, `OnPlayerDeathEvent`. 물리·지속·낙하 피해 처리 개입. | 사망과 패배를 분리해야 합니다. 이 API는 단순 문자열 ModEvent라고 가정하면 안 됩니다. `M/Acheron - Death Alternative/Source/Scripts/Acheron.psc:23–57`; [Hooks.cpp](https://github.com/Scrabx3/Acheron/blob/main/src/Acheron/Hooks/Hooks.cpp). |
| **CalamityAffixes v2.3.1** `P:29` | 세 HealthDamage vtable 훅과 추가 피해·변환·지연 처리. 문서에 Hit/DotApply 브리지 이름도 있으나 현재 송신 경로는 별도 확인 필요. | 훅 공존 최우선 대상입니다. 공격 시도와 추가 발동 피해를 분리해야 합니다. [v2.3.1 Hooks.cpp](https://github.com/servaltullius/calamity-reactive-loot-affixes/blob/v2.3.1/skse/CalamityAffixes/src/Hooks.cpp). |
| **UAPNG** `P:1212–1215` | `DrinkPotionStart`, `DrinkPotion_Slow`, `DrinkPotion_NoMove`; `AnimObjLoad`+`AnimObjectDrinkPotion`, 종료 계열 이벤트. | 포션 사용 요청·동작·소비 시점의 차이를 검증할 대상입니다. [애니메이션 sink](https://github.com/QY-MODS/SKSE_UAPNG/blob/main/src/animation_event_sink.cpp). |

추가 조사 대상도 남습니다.

- **Elden Rim 계열:** `P:33–40`에서 활성 확인. 조사한 폴더의 loose `.psc`에서는 관련 송신 코드를 확보하지 못했습니다. **태그가 없다는 뜻은 아닙니다.** 컴파일 스크립트·behavior·공개 소스 버전의 추가 확인이 필요합니다.
- **Dismembering Framework, Next-Gen Decapitations, Core Impact Framework, Mu Impact Framework:** 설치 DLL은 확인했지만, 이번 조사에서 설치 버전에 대응하는 공개 피해 훅·이벤트 계약을 확보하지 못했습니다.
- **Killmove Fixes:** 확인한 폴더는 `EVE - KillMove Fixes.esp`입니다. 이를 SKSE 피해 훅 DLL로 분류하면 안 됩니다.
- **TrueHUD:** 공개 소스에서 같은 HealthDamage 세 슬롯 훅을 확인했습니다. 프로필에는 기본 TrueHUD와 별도 DLL 제공 패치가 있어 실제 승자 확인이 필요합니다. `P:4439–4440`.
- **Unpoisoned Blocking:** 공개 소스는 마법 효과 추가 경로를 훅합니다. 직접 같은 HealthDamage 슬롯을 훅한다고 확인한 것은 아닙니다. [Hooks.hpp](https://github.com/Seb263/SkyrimSE_UnpoisonedBlocking/blob/main/src/Hooks.hpp).

따라서 피해 처리 후보는 **같은 슬롯 확인: Calamity·TrueHUD**, **인접 피해 경로 확인: Precision·Maxsu Poise·Parry for All·Acheron·Unpoisoned Blocking**, **설치 확인만 된 추가 조사 대상**으로 나누는 것이 맞습니다.

**7. 목록에서 빠진 활동**

아래는 기존의 포괄적인 “전투·제작·퀘스트” 행에 묻어 두면 실제로 누락되기 쉬운 활동입니다. **누락 여부는 C와의 비교 사실**, 소스는 **구현 제안**이며, 발화·정확도는 별도 검증해야 합니다.

| 심각도·추가 위치 | 빠진 활동 | 제안 소스·근거 |
|---|---|---|
| 높음 §2 | 근접 공격 시도, 헛손질, 공격 취소, 무기 뽑기·넣기 | SKSE ActionEvent와 그래프 이벤트. `include/SKSE/Events.h:42–52`. |
| 높음 §2 | 방어 자세 시작·종료, 가드 브레이크, 패링 성공/실패, 무기끼리 충돌 | Parry for All·MSL·Maxsu Block·Precision별 신호. 위 모드 표 참조. |
| 높음 §2 | 적중했지만 피해 0, 면역·무적에 막힘, 반사·추가타·지속 피해 | Hit/피해/효과/모드 콜백을 분리. `H/H/HitData.h:76–87`, Calamity·Parry 공개 소스. |
| 높음 §2·3 | 회복량, 흡혈 회복, 체력·매지카·기력 소모와 회복 | AV 관측과 효과 문맥. 피해 훅만으로 회복을 포괄한다고 가정하지 말 것. `H/A/ActorValueOwner.h:16–18`. |
| 중간 §2 | 비틀거림 종료, 넉다운·일어나기, 마비·공포·진정·항복 | 그래프 상태, 활성 효과, Actor 상태 전이. `H/A/ActorState.h:163,201`, `H/M/MagicTarget.h:122`. |
| 높음 §12 | 패배했지만 살아남음, 구조, 부활·재소환 | Acheron API, life-state 전이, 소환 관계. 로컬 `Acheron.psc:23–57`. |
| 중간 §3 | 집중 주문 유지·중단, 이중 시전, 두루마리·지팡이·종족 능력 사용 | ActionEvent, MagicCaster 상태, 사용 form 유형. [OnSpellCast](https://ck.uesp.net/wiki/OnSpellCast_-_ObjectReference). |
| 중간 §3 | 소환수 사망·소멸·교체, 시체 소생·지배와 해제 | 효과·소환/지배 소유관계·사망. `H/A/Actor.h:633`, `H/S/SoulsTrapped.h:16`만으로 소환 생애는 알 수 없음. |
| 중간 §3 | 단어 해금·영혼 소비, 영혼석 충전 결과, 장비 충전·고갈 | 단어/외침 상태와 영혼 AV·인벤토리 extra data 대조. `H/I/InventoryEntryData.h:41`, QueryStat의 해금·영혼석 항목. |
| 중간 §4 | 흡혈, 늑대인간 포식, 식인, 변신 해제·치료 | 통계와 해당 효과·퀘스트. `S/Survival_HungerEatingDetection.psc:27–38`, `Necks Bitten`, `Mauls`, `Vampirism Cures`. |
| 중간 §5 | 기술 전설화, 퍽 환불·재분배, 흡혈군주·늑대인간 퍽 | `PlayerSkills::legendaryLevels`, 퍽 rank 비교, 특수 통계. `H/P/PlayerCharacter.h:579`, 로컬 DLC1 스크립트. |
| 중간 §7 | 재료 맛보기와 효과 발견, 마법부여 분해, 제련·무두질 | 소비 form 유형, 제작 메뉴·인벤토리·학습 상태. 일반 음식/포션·제작 행과 별도 분류. QueryStat `Ingredients Eaten`, `H/I/ItemCrafted.h:16`. |
| 중간 §6·7 | 보관·꺼내기, 장비 개명, 퀘스트 물품 회수, 물품 파괴·소비 | 컨테이너 이동 방향+메뉴 문맥+extra data. `H/T/TESContainerChangedEvent.h:11`, `H/I/InventoryEntryData.h:66`. |
| 중간 §8 | 설득·위협·뇌물의 시도와 성공, 주먹다짐 승리 | 선택 INFO와 결과·통계 대조. `MenuTopicManager`, `Persuasions/Intimidations/Bribes/Brawls Won`; `S/QF_DGIntimidateQuest_00047AE6.psc:115`. |
| 중간 §8 | 동료에게 대기·따라오기·이동·공격·물건 집기 명령, 고용료 | 명령 메뉴·대화·AI package/퀘스트 어댑터. teammate 변화만으로는 명령을 알 수 없음. `H/A/Actor.h:657`. |
| 중간 §6·8 | 여관 방 대여, 상점 투자, 기부·구걸 응답, 집 꾸미기·가족 선물 | 대화/퀘스트와 실제 금화·물품 이동. `S/PerkInvestorQuestScript.psc:17`, `S/HousePurchaseScript.psc:28`. |
| 중간 §9 | 말 절도, 소매치기 실패·역소매치기, 장물 압수·회수, 벌금 지불·저항 | 범죄별 통계, 관할, 대화, 인벤토리 이동. 현상금 순변화만으로 구분 불가. QueryStat 및 `H/T/TESFaction.h:158`. |
| 중간 §1 | 배 이동, 드래곤 탑승, 포털·스크립트 순간이동, 문·레버·퍼즐 조작 | 활성화+셀/월드 전환+퀘스트 문맥. `H/T/TESActivateEvent.h:13`, `H/P/PositionPlayerEvent.h:9`. |
| 중간 §7 | 낚시 시작·실패·중단·물고기/잡동사니 획득·전시 | CC 낚시 시스템 어댑터+물품 이동. `S/ccBGSSSE001_RadiantFishEventListener.psc:11–16`에 catch listener가 있음. **일반 SKSE ModEvent라는 뜻은 아님.** |
| 중간 §7 | 하스파이어 토지·증축·가구 제작, 경작·수확, 가축·양식장·가족 활동 | BYOH 시스템과 제작·활성화·인벤토리 관측. `S/BYOHWorkbenchScript.psc`, `S/BYOHFishHatcheryContainerScript.psc`, `S/BYOHRelationshipAdoptionPlayerScript.psc`. |
| 중간 §7·11 | CC 야영지 설치·철거, 농장 운영·수익, 반려동물 명령 | 해당 CC의 퀘스트·스크립트 어댑터와 활성화·이동 사건. 범용 제작/동료 행만으로 의미를 확정하지 말 것. 개별 어댑터는 미검증. |
| 중간 §13 | 같은 장소에서의 날씨 전환·해질녘·시간 경과, 장시간 체류의 중단 | Sky/Calendar 변경 관측, 일시정지·메뉴·AFK 제외 규칙. `H/S/Sky.h:105,133`, `H/C/Calendar.h:66–76`. |

추가로 이 모드팩은 **SkyParkour, Paraglider, Simple Diving, HorsePower**가 활성입니다. 바닐라 이동만 기록하면 도약·등반·활공·잠수·탑승 행동이 빠집니다. 이는 바닐라 필수 범위와 별개인 **팩 전용 어댑터 항목**으로 추가해야 합니다. 근거: `P:1034,1089,1216–1220`.

**8. 기록량·성능 추정**

실측은 하지 않았습니다. 다음은 설계 용량을 판단하기 위한 **가정 계산**입니다.

사건 하나를 UTF-8 JSONL **512–1,024바이트**로 가정하면:

| 평균 저장 사건/초 | 시간당 원시 저장량 | 100시간 |
|---:|---:|---:|
| 2 | 약 3.7–7.4MB | 0.37–0.74GB |
| 10 | 약 18–37MB | 1.8–3.7GB |
| 50 | 약 92–184MB | 9.2–18.4GB |
| 200 | 약 369–737MB | 36.9–73.7GB |
| 2,000 | 약 3.7–7.4GB | 369–737GB |

메타데이터 반복, `annot`, pending/봉인 파일의 중복 보유, discarded, DB·색인은 별도입니다. 실제 평균 레코드 크기를 측정하기 전의 범위입니다.

입력 사건률은 다음처럼 잡아 시험해야 합니다.

- **평상시 탐험:** 저장 사건 평균 1–10/s를 초기 가정으로 둘 수 있으나, 모드의 효과·장비 변경에 따라 달라집니다.
- **대도시:** 모든 NPC의 애니메이션·장비·효과를 받으면 수백~수천 콜백/s를 시험 범위에 넣어야 합니다. 저장 필터를 통과하지 않아도 sink 호출 비용은 발생합니다.
- **10–40명 전투:** 가정상 40명이 초당 1–2회 타격하고 타격당 관련 관측 3–8개가 생기면 **120–640/s**입니다. 여기에 지속 효과를 더해야 합니다.
- **지속 피해 스트레스:** 30개의 활성 피해 경로가 초당 60번 관측된다는 가정이면 그것만 **1,800/s**입니다. 실제 엔진 호출 빈도는 측정 대상이며 이 숫자는 관측 결과가 아닙니다.

**[높음] C 전체, D:125,243–263 — 초당 200건 저장 제한은 콜백 비용 제한이 아닙니다.** 다음 위험이 있습니다.

1. **늦은 필터링:** 이름 조회·폼 해석·JSON 생성 후 버리면 이미 비용을 지불한 것입니다. 숫자·핸들 수준에서 관련성을 먼저 판단해야 합니다.
2. **대상 전체 순회:** 모든 actor×모든 actor 탐지 비교, 모든 퍽·팩션·인벤토리의 반복 순회는 피해야 합니다. 알려진 대상 집합을 유지하십시오.
3. **이벤트 소스 잠금:** `BSTEventSource::SendEvent`는 잠금 안에서 sink를 호출합니다. 싱크의 20µs가 단순히 기록기 내부 비용으로 끝나지 않습니다. 근거: `H/B/BSTEvent.h:131–133`.
4. **프레임 집중:** 사건당 20µs가 200건 한 프레임에 몰리면 **4ms**입니다. 평균 사건률과 콜백 p99만으로 프레임 영향이 작다고 보장할 수 없습니다.
5. **4MB 큐:** 200/s×512–1,024B이면 writer가 멈췄을 때 대략 **20–40초**, 2,000/s이면 **2–4초** 수준입니다. 메모리 내 실제 구조 크기까지 적용하면 달라집니다.
6. **critical 승격 폭주:** 지속 피해가 계속 큰 피해·위기로 승격되거나 반복 사망이 중복 기록되면 detail 접기로 막을 수 없습니다.
7. **메뉴 중 타이머:** 일시정지 시간을 체류·조우 지속 시간·탐지 지속 시간에 그대로 넣으면 활동 해석이 오염됩니다.
8. **복구 경계:** 로드·세대 전환 때 폴링 기준선을 재설정하지 않으면 퍽 제거·범죄 감소·물품 획득 같은 가짜 사건이 생깁니다.

일반적인 폴링 시작점은 **플레이어 이동 상태 2–4Hz, 생존·관계 0.2–1Hz, 대화 자막 10Hz, 금화·퍽은 이벤트/메뉴 경계 중심**입니다. 동료·주변 NPC는 대상 수와 조회 작업 수에 상한을 두고 분산해야 합니다. 킬무브·패링·짧은 회피는 저빈도 폴링으로 해결할 항목이 아닙니다.

**목록에서 결정을 바꿔야 할 항목**

| 항목 | 지금 결정 | 제안 | 근거 |
|---|---|---|---|
| 원시 활동과 묶음 | 개별 사건을 버리고 합계만 저장 | 원시 보관과 AI용 압축을 분리. 손실 모드는 명시 | C:13, D:125와 사용자 요구 충돌 |
| 피해량·전후 체력 | HealthDamage 훅으로 확정 | 보고값·관측 체력·검증된 실제 감소량을 분리 | 함수 인자·공개 구현의 시점 차이 |
| 훅 공존 | 0단계에서 확인 | Calamity·TrueHUD 설치 순서와 체인 보존을 필수 조건으로 명시 | Calamity v2.3.1의 선행 훅 거부 |
| 공격·빗나감 | 적중 중심, 연결 없으면 빗나감 추정 | 공격 시도 추가, 미연결은 `unresolved` | ActionEvent 존재, 일대일 연결 불가 |
| 막기·경직 | HitData로 기록 | 실제 동작 결과와 모드 포이즈 채널 분리 | Maxsu Poise의 stagger 초기화 |
| 회피·패링 | 일반 애니메이션 태그 | 모드별 요청·시작·판정 창·성공·종료 계약 | UND·Parry for All의 직접 신호 |
| 포션·음식·독 | 한 소모품 equip 경로 | 포션/음식 근거 유지, 독 별도 검증, 소비 시점 분리 | SMI·UAPNG·PoisonedWeapon 한계 |
| 범죄·훈련 통계 | TrackedStats 이벤트 | 통계별 이벤트/조회 지원표와 기준선 대조 | 모든 통계가 발화하지 않음 |
| 거래 | 금화·물품 순변화 | 개별 거래 전송 추적, 가격 미상 허용 | 왕복 거래·동시 변경 복원 불가 |
| 자물쇠 | LockChanged=따기 | LocksPicked+메뉴·대상 문맥 | 열쇠·스크립트·강제 개방과 혼동 |
| 동료·탈것 | teammate·소유 말 중심 | 동료·소환수·반려동물·최근 탑승·소유권 분리 | 해당 API의 의미 차이 |
| 사망·처형 | 마지막 피해+사망 | 패배·구조·킬무브 구간·사망을 분리 | Acheron 활성, 사망 payload 한계 |
| 숨은 퀘스트 | 제외 | 서사 출력에서는 제외하되 탐지 소스로 허용 | 생활·가족 시스템 누락 방지 |
| 콘솔·치트 | 해당 사건을 콘솔로 표시해 제외 | 관측 당시 상태와 원인 확신도를 분리 | 콘솔 열림≠콘솔 원인 |
| 분위기·난이도 | 장면 시작 스냅숏, 설정은 안 함 | 날씨 전환 기록, 난이도·모드 설정은 기술 메타데이터로 보관 | 긴 장면·피해 해석의 재현성 |

**구현 전에 꼭 확인할 것 — 상위 5개**

1. **피해 훅의 실제 적용 경계와 공존:** 1.6.1170에서 Actor/Character/PlayerCharacter 경로, 피해 부호, 사전·사후 체력, Calamity·TrueHUD 설치 순서를 대조해야 합니다.
2. **적중·피해·방어의 연결 정확도:** 일반 무기, 부여마법, 독, 지속 주문, 반사·추가타, 패링 무효화에서 중복·오귀속·누락을 측정해야 합니다.
3. **소모품과 범죄 통계의 실제 발화표:** 포션·음식·독의 확정/취소/UAPNG 경로와 통계별 누적값·이벤트 발생 여부를 구분해야 합니다.
4. **팩 전용 사건 계약과 실제 승자:** TKDR·UND·Parry·MSL·Acheron의 시작/성공/종료 의미, DLL·behavior의 VFS 승자, 공개 소스와 설치 버전의 대응을 고정해야 합니다.
5. **완전성 기준과 부하 시험:** “모든 활동”의 대상 범위와 허용 손실을 먼저 정하고, 대도시·대규모 전투·긴 플레이에서 입력률, 저장률, 접힌 사건, 큐 점유, 프레임 지연을 함께 측정해야 합니다.

이번 **정적 검토는 완료**했습니다. 다만 독의 정확한 이벤트 경로, 일부 모드의 설치 버전별 신호, 피해 적용 시점과 성능은 확인되지 않았으므로 **전체 활동의 무손실 기록 가능성이 검증된 상태는 아닙니다.**