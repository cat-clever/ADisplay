package com.adisplay.tv.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.runtime.withFrameNanos
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.focus.focusProperties
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.tv.material3.MaterialTheme
import androidx.tv.material3.Surface
import androidx.tv.material3.Text
import com.adisplay.tv.EngineModel
import com.adisplay.tv.Language
import com.adisplay.tv.QualityPreset

/**
 * 界面入口：按「有没有媒体在播」在待机页与播放页之间切，并负责那两块抽屉。
 *
 * 不另开 Activity —— 文档 2.3 要求投屏开始时自动全屏并置于最上层、结束后回到
 * 待机页，同一个 Activity 里换内容比走 Intent 更直接，也少一层「切回来时
 * 还剩下多少状态」的麻烦。切换的唯一依据是 model.playingMedia：核心说会话
 * 开着就有媒体在播，说关了就没有，界面不自己猜。
 *
 * 抽屉放在这里而不是各自的页面里：日志在待机页与播放页都要能看（投屏中出问题
 * 时正是最该看它的时候），改名的入口只在待机页。抽屉盖在最上层，两个页面共用
 * 同一套开合状态，不用各自记一份。
 */
@Composable
fun StandbyScreen(model: EngineModel) {

    // 日志抽屉开着没有。改名的开合不用另记：草稿在模型里（model.nameDraft），
    // 核心那边要拿它校验名称，界面跟着它走就行。
    var logOpen by remember { mutableStateOf(false) }

    // 抽屉里的字号沿用待机页那套分档参数：两边各写一份，改了一处忘了另一处，
    // 同一块日志在抽屉里和（曾经）页面上的会长得不一样。
    val layout = resolveStandbyLayout(LocalConfiguration.current.screenWidthDp)

    Box(modifier = Modifier.fillMaxSize()) {

        // 「结束投屏」只回主界面，**不停接收服务** —— 服务一停广播就撤了，
        // 手机那边立刻找不到这台设备。核心那边的会话还开着（JNI 面上没有
        // 「只关掉某一个会话」的接口），所以界面自己记着「这一次我不看了」；
        // 手机再推一条（castEpisode +1）就重新进播放页。见 EngineModel。
        val dismissed = model.isCastingDismissed()
        val media = model.playingMedia
        val mirrorAlive = model.mirrorSessionId != null
        val showMirror = mirrorAlive && !dismissed
        val showPlayback = !mirrorAlive && media != null && !dismissed

        // 镜像画面：**只要会话还在就一直留在树上**，哪怕用户已经点了「结束投屏」。
        //
        // 不能让它随页面离开组合：一离开，SurfaceView 的 surface 就被销毁，
        // MirrorVideoPlayer.detach() 跟着把解码器释放掉；「继续观看」回来是**新建
        // 解码器**，必须等发送端下一个 I 帧才出画面 —— 手机画面静止时 iOS 可能很久
        // 不插 I 帧，用户看到的就是「一直黑屏但有声音」（2026-10-11 报的）。
        // active=false 时它只是缩成 1dp 挪到角落：surface 与解码器都还在，
        // 回来那一刻画面就是现成的。（Windows 一直是这个行为 —— 它的位图一直在更新，
        // 所以那边恢复是即时的。）
        if (mirrorAlive) {
            MirrorScreen(
                model = model,
                onExit = { model.dismissCasting() },
                onShowLog = { logOpen = true },
                active = showMirror,
            )
        }
        // 这里再判一次 media 而不是用 !!：判过之后编译器就能智能转换，
        // 下面直接用 playbackMedia.sessionId 即可，不必用一个断言把空值风险藏起来。
        val playbackMedia = media
        if (!mirrorAlive && !dismissed && playbackMedia != null) {
            PlaybackScreen(
                model = model,
                sessionId = playbackMedia.sessionId,
                url = playbackMedia.url,
                onExit = { model.dismissCasting() },
                onShowLog = { logOpen = true },
            )
        }
        if (!showMirror && !showPlayback) {
            StandbyContent(
                model = model,
                onShowLog = { logOpen = true },
            )
        }

        // 结束投屏之后，发送端往往还在推流 —— 给两个入口：回去看，或者真正断开。
        //
        // 盖在底部、而不是排进待机页的 Column：那一页的高度是算着放的，插一行
        // 进去会把下面的内容挤出可视区。
        val resumable = model.mirrorSessionId != null || model.playingMedia != null
        if (dismissed && resumable) {
            ResumeBanner(
                model = model,
                layout = layout,
                modifier = Modifier
                    .align(Alignment.BottomCenter)
                    .padding(bottom = layout.spacingLoose * 3),
            )
        }

        // 抽屉组合在页面之后，所以返回键的处理也排在页面之后 —— 抽屉开着时按
        // 返回先关抽屉，而不是把投屏停掉（见 Drawer）。
        if (logOpen) {
            LogDrawer(
                lines = model.logs,
                textStyle = layout.logStyle,
                buttonTextStyle = layout.actionStyle,
                onClear = { model.clearLogs() },
                onDismiss = { logOpen = false },
            )
        }

        // 改名抽屉。竖屏按内容取高（就三行），横屏占满高度、宽度占一块 ——
        // 横屏从侧边抽，高度不占满的话上面会空出来一大截。
        if (model.nameDraft != null) {
            Drawer(
                onDismiss = { model.cancelRename() },
                modifier = if (isLandscape()) {
                    Modifier.fillMaxHeight().fillMaxWidth(0.42f)
                } else {
                    Modifier.fillMaxWidth()
                },
            ) {
                NameEditor(model = model, layout = layout)
            }
        }
    }
}

/**
 * 待机页。
 *
 * 文档 2.3 的电视端要求：
 *   * 10 英尺界面 —— 大字号，遥控器方向键与「确定 / 返回」键完成全部操作。
 *   * 显示设备名称、局域网 IP 和连接说明，方便手机用户找到设备。
 *
 * 排版参数由 resolveStandbyLayout 按屏幕宽度分档给出，不写死尺寸 ——
 * 电视端的屏幕跨度太大（手机横屏 640dp、1080p 电视 960dp、4K 电视 1920dp），
 * 一套固定字号必然在某一档上不合适。
 *
 * 日志与改名都不在这一页里常驻，收进抽屉（见 StandbyScreen）：常驻会把上面的
 * 按钮往上挤，而这一页的高度是算着放的。
 */
/// 待机页底部那条「还能回去」的横幅。
///
/// 只在「用户结束了本地播放、而且会话还在」时出现 —— 会话真的没了就该消失，
/// 否则会变成一个点了没反应的按钮。
@Composable
private fun ResumeBanner(
    model: EngineModel,
    layout: StandbyLayout,
    modifier: Modifier = Modifier,
) {
    val resumeFocus = remember { FocusRequester() }
    val disconnectFocus = remember { FocusRequester() }

    // 用户刚按过「结束投屏」，下一步最可能按的就是「继续观看」—— 把初始焦点给它，
    // 省得还要按方向键去找。
    //
    // ★ 不按设备类型区分（原来只在 LEANBACK / TELEVISION 上报时才抢焦点）。
    //   有些电视盒子、以及手机接电视的场景根本不上报这些标志，会被误判成触屏，
    //   于是「谁也不要焦点」：方向键按下去没有任何反应，横幅上的两个按钮永远
    //   点不到，只能去关服务。抢焦点在触屏上最多多一圈边框，代价远小于遥控器
    //   完全用不了。
    LaunchedEffect(Unit) {
        // 横幅是覆盖层，首次组合时按钮可能还没有完成测量；等一帧后再请求，
        // 并短暂重试，避免部分电视盒子上 requestFocus() 静默返回 false。
        repeat(3) {
            withFrameNanos { }
            resumeFocus.requestFocus()
            return@LaunchedEffect
        }
    }

    val panel = modifier
        .clip(RoundedCornerShape(12.dp))
        // 与抽屉同一套观感：半透明面板，底下的待机页透出来一点。
        .background(MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.94f))
        .padding(horizontal = 20.dp, vertical = 12.dp)

    // 竖屏（窄屏）上「一句话 + 两个按钮」挤在一行，最右边的「断开投屏」会被顶出屏幕
    // —— 待机页那三个按钮、画质那一档都出过同样的错。竖屏改成上下两段：说明一行、
    // 两个按钮等分整行宽度；横屏空间够，保持原来的一行。
    if (isLandscape()) {
        Row(
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.Center,
            modifier = panel,
        ) {
            Text(
                text = LocalStrings.current.castingEndedBanner,
                style = layout.bodyStyle,
                modifier = Modifier.padding(end = 16.dp),
            )
            BannerButtons(model, layout, resumeFocus, disconnectFocus, equalWidth = false)
        }
    } else {
        Column(
            horizontalAlignment = Alignment.CenterHorizontally,
            modifier = panel.fillMaxWidth(),
        ) {
            Text(
                text = LocalStrings.current.castingEndedBanner,
                style = layout.bodyStyle,
            )
            Spacer(modifier = Modifier.height(layout.spacingTight))
            Row(
                verticalAlignment = Alignment.CenterVertically,
                modifier = Modifier.fillMaxWidth(),
            ) {
                BannerButtons(model, layout, resumeFocus, disconnectFocus, equalWidth = true)
            }
        }
    }
}

/// 横幅上那两个按钮。
///
/// 抽出来是因为竖屏与横屏的排法不同、而按钮本身一样 —— 抄两份迟早会改漏一处。
/// 写成 RowScope 的扩展，好在需要等分宽度时直接用 weight。
@Composable
private fun RowScope.BannerButtons(
    model: EngineModel,
    layout: StandbyLayout,
    resumeFocus: FocusRequester,
    disconnectFocus: FocusRequester,
    equalWidth: Boolean,
) {
    val strings = LocalStrings.current
    val buttonModifier = if (equalWidth) Modifier.weight(1f) else Modifier

    ActionButton(
        text = strings.resumeCasting,
        textStyle = layout.actionStyle,
        minWidth = 0.dp,
        modifier = buttonModifier.focusProperties {
            right = disconnectFocus
        },
        focusRequester = resumeFocus,
        onClick = { model.resumeCasting() },
    )
    Spacer(modifier = Modifier.width(12.dp))
    ActionButton(
        text = strings.disconnectCasting,
        textStyle = layout.actionStyle,
        minWidth = 0.dp,
        modifier = buttonModifier.focusProperties {
            left = resumeFocus
        },
        focusRequester = disconnectFocus,
        onClick = { model.disconnectCasting() },
    )
}

/// 画质档位（文档 2.3 的「低端盒子画质分档」）。
///
/// 它决定的是**手机按多大分辨率编码屏幕**：核心在每次 /info 应答时把这个尺寸
/// 给出去，所以只影响下次连接 —— 当前这一路镜像不受影响，也不必重启服务。
/// 这一句必须写在界面上：否则用户在投屏中改档位、看到画面毫无变化，会以为
/// 这个开关坏了。
///
/// 排成按钮行下面独立的一行，而不是塞进那一行里加第四个按钮：那三个已经用
/// weight 占满整行，加进去会在窄屏上把字挤没（那一行上出过一次同样的错，
/// 见 StandbyContent 里的注释）。这一行在按钮之下，矮屏上被裁掉的是它而不是
/// 按钮 —— 与连接说明一样，本来就排在「必须先看见」的内容之后。
@Composable
private fun QualityRow(
    model: EngineModel,
    layout: StandbyLayout,
    modifier: Modifier = Modifier,
) {
    // 每个档位一个 FocusRequester（ActionButton 要求必填）。档位是固定三项，
    // 所以这个列表的长度在组合期间恒定，remember 放在 map 里是安全的。
    val strings = LocalStrings.current
    val focuses = QualityPreset.entries.map { remember { FocusRequester() } }

    Column(
        modifier = modifier.fillMaxWidth(),
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Text(
            text = strings.mirrorQuality,
            style = layout.bodyStyle,
        )

        Spacer(modifier = Modifier.height(layout.spacingTight))

        Row(
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.Center,
            modifier = Modifier.fillMaxWidth(),
        ) {
            QualityPreset.entries.forEachIndexed { index, preset ->
                if (index > 0) {
                    Spacer(modifier = Modifier.width(10.dp))
                }
                ActionButton(
                    // 按钮上只写档位名，分辨率放到下面那行说明里 —— 竖屏（窄屏）
                    // 上「高清 2160p」这三个挤在一行会把最后一个挤出屏幕，这一行
                    // 出过一次同样的错（见上面三个按钮的注释）。
                    text = strings.qualityLabel(preset),
                    textStyle = layout.actionStyle,
                    minWidth = 0.dp,
                    // 等分整行宽度：三个按钮的宽度跟着屏幕走，窄屏上会一起变窄
                    // 而不是把最右边那个顶出去。
                    modifier = Modifier.weight(1f),
                    focusRequester = focuses[index],
                    selected = model.qualityPreset == preset,
                    // 核心库没起来时压暗且不可聚焦，与上面那三个按钮一致。
                    enabled = model.isNativeAvailable,
                    onClick = { model.applyQualityPreset(preset) },
                )
            }
        }

        Spacer(modifier = Modifier.height(layout.spacingTight))

        Text(
            text = strings.qualityHint(
                QualityPreset.SMOOTH.resolution,
                QualityPreset.BALANCED.resolution,
                QualityPreset.SHARP.resolution,
            ),
            style = layout.bodyStyle,
            textAlign = TextAlign.Center,
            modifier = Modifier
                .fillMaxWidth()
                .widthIn(max = 900.dp)
        )
    }
}

/// 界面语言那一行。
///
/// 「中文」与「English」两个词**不翻译** —— 任何语言下都这么写，用户找的是自己认得的
/// 那两个字，而不是当前语言的译名（通行做法，也省掉「English 用中文怎么写」这种问题）。
///
/// 排法与「镜像画质」那一行一致（标签一行、按钮等分整行宽度），理由见那里的注释：
/// 窄屏上等分宽度不会把最右边的按钮顶出屏幕。
@Composable
private fun LanguageRow(
    model: EngineModel,
    layout: StandbyLayout,
    modifier: Modifier = Modifier,
) {
    val strings = LocalStrings.current
    val chineseFocus = remember { FocusRequester() }
    val englishFocus = remember { FocusRequester() }

    Column(
        modifier = modifier.fillMaxWidth(),
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Text(text = strings.uiLanguage, style = layout.bodyStyle)

        Spacer(modifier = Modifier.height(layout.spacingTight))

        Row(
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.Center,
            modifier = Modifier.fillMaxWidth(),
        ) {
            ActionButton(
                text = "中文",
                textStyle = layout.actionStyle,
                minWidth = 0.dp,
                modifier = Modifier.weight(1f),
                focusRequester = chineseFocus,
                selected = model.language == Language.ZH,
                onClick = { model.applyLanguage(Language.ZH) },
            )
            Spacer(modifier = Modifier.width(10.dp))
            ActionButton(
                text = "English",
                textStyle = layout.actionStyle,
                minWidth = 0.dp,
                modifier = Modifier.weight(1f),
                focusRequester = englishFocus,
                selected = model.language == Language.EN,
                onClick = { model.applyLanguage(Language.EN) },
            )
        }
    }
}

/// 广播网卡：设备发现用哪几块网卡。
///
/// 按钮上只放短词（「自动」「含虚拟网卡」），完整说明放下面那行提示里 ——
/// 与「镜像画质」那一行同一个做法：窄屏上等分宽度的两个按钮放不下长句子。
@Composable
private fun BroadcastRow(
    model: EngineModel,
    layout: StandbyLayout,
    modifier: Modifier = Modifier,
) {
    val strings = LocalStrings.current
    val autoFocus = remember { FocusRequester() }
    val includeFocus = remember { FocusRequester() }

    Column(
        modifier = modifier.fillMaxWidth(),
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Text(text = strings.broadcastLabel, style = layout.bodyStyle)

        Spacer(modifier = Modifier.height(layout.spacingTight))

        Row(
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.Center,
            modifier = Modifier.fillMaxWidth(),
        ) {
            ActionButton(
                text = strings.broadcastAuto,
                textStyle = layout.actionStyle,
                minWidth = 0.dp,
                modifier = Modifier.weight(1f),
                focusRequester = autoFocus,
                selected = !model.includeVirtualInterfaces,
                onClick = { model.applyIncludeVirtualInterfaces(false) },
            )
            Spacer(modifier = Modifier.width(10.dp))
            ActionButton(
                text = strings.broadcastIncludeVirtual,
                textStyle = layout.actionStyle,
                minWidth = 0.dp,
                modifier = Modifier.weight(1f),
                focusRequester = includeFocus,
                selected = model.includeVirtualInterfaces,
                onClick = { model.applyIncludeVirtualInterfaces(true) },
            )
        }

        Spacer(modifier = Modifier.height(layout.spacingTight))

        // 样式与画质那行的提示一致（StandbyLayout 里没有单独的 hintStyle）。
        Text(
            text = strings.broadcastHint,
            style = layout.bodyStyle,
            textAlign = TextAlign.Center,
            modifier = Modifier
                .fillMaxWidth()
                .widthIn(max = 900.dp)
        )
    }
}

@Composable
private fun StandbyContent(model: EngineModel, onShowLog: () -> Unit) {

    val configuration = LocalConfiguration.current
    val layout = resolveStandbyLayout(configuration.screenWidthDp)
    val inputMode = rememberInputMode()
    val strings = LocalStrings.current

    val buttonFocus = remember { FocusRequester() }
    val logFocus = remember { FocusRequester() }
    val renameFocus = remember { FocusRequester() }
    val hotspotFocus = remember { FocusRequester() }
    val settingsFocus = remember { FocusRequester() }
    var advancedOpen by remember { mutableStateOf(false) }

    Surface(
        modifier = Modifier.fillMaxSize(),
    ) {
        Column(
            modifier = Modifier
                .fillMaxSize()
                .background(MaterialTheme.colorScheme.background)
                .padding(
                    horizontal = layout.horizontalPadding,
                    vertical = layout.verticalPadding
                ),
            horizontalAlignment = Alignment.CenterHorizontally
        ) {
            // 内容整体居中，并且可以滚动 —— 矮屏上放不下时裁掉的是最下面那行
            // 提示，而按钮在块的顶部，一定看得见。
            Column(
                modifier = Modifier
                    .weight(1f)
                    .verticalScroll(rememberScrollState()),
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.Center
            ) {
                Box(
                    modifier = Modifier
                        .fillMaxWidth()
                        .widthIn(max = 1100.dp)
                        .clip(RoundedCornerShape(24.dp))
                        .background(MaterialTheme.colorScheme.surfaceVariant)
                        .border(BorderStroke(1.dp, MaterialTheme.colorScheme.outline), RoundedCornerShape(24.dp)),
                ) {
                    Column(
                        modifier = Modifier.padding(horizontal = 28.dp, vertical = 24.dp),
                        horizontalAlignment = Alignment.CenterHorizontally,
                    ) {
                        Text(
                            text = strings.welcomeTitle,
                            style = layout.titleStyle,
                            textAlign = TextAlign.Center,
                        )
                        Spacer(modifier = Modifier.height(layout.spacingTight))
                        Text(
                            text = strings.welcomeSubtitle,
                            style = layout.bodyStyle,
                            textAlign = TextAlign.Center,
                            color = Color.White.copy(alpha = 0.78f),
                            modifier = Modifier.widthIn(max = 700.dp),
                        )
                        Spacer(modifier = Modifier.height(layout.spacingMedium))
                        Text(
                            text = model.deviceName,
                            style = layout.statusStyle,
                            textAlign = TextAlign.Center,
                            color = MaterialTheme.colorScheme.primary,
                        )
                        Spacer(modifier = Modifier.height(layout.spacingTight))
                        Text(
                            text = model.statusText,
                            style = layout.bodyStyle,
                            textAlign = TextAlign.Center,
                            color = if (model.serviceEnabled) Color(0xFF9BE7B5) else Color.White.copy(alpha = 0.68f),
                        )
                    }
                }

                // 按钮紧跟状态文字、排在说明之前 —— 矮屏上它必须在首屏可见区内。
                Spacer(modifier = Modifier.height(layout.spacingMedium))

                // 主操作单独占一行，日志和改名作为次操作并排放置。电视端最重要的
                // 动作必须有明确层级，不能把三个按钮做成同样重量的一排。
                Box(
                    modifier = Modifier
                        .fillMaxWidth()
                        .widthIn(max = 1100.dp)
                        .clip(RoundedCornerShape(20.dp))
                        .background(Color.White.copy(alpha = 0.055f)),
                ) {
                    Column {
                        ActionButton(
                            text = if (model.serviceEnabled) LocalStrings.current.stopReceiving
                                   else LocalStrings.current.startReceiving,
                            textStyle = layout.actionStyle,
                            minWidth = 0.dp,
                            modifier = Modifier
                                .fillMaxWidth()
                                .padding(horizontal = 12.dp, vertical = 10.dp),
                            focusRequester = buttonFocus,
                            primary = true,
                            // 核心库没加载起来时按钮压暗且不可聚焦：让用户看出是这一块
                            // 坏了，而不是对着一个按了没反应的按钮猜网络。
                            enabled = model.isNativeAvailable,
                            onClick = { model.toggleService() },
                        )

                        Row(
                            verticalAlignment = Alignment.CenterVertically,
                            horizontalArrangement = Arrangement.spacedBy(12.dp),
                            modifier = Modifier
                                .fillMaxWidth()
                                .padding(horizontal = 12.dp, vertical = 0.dp),
                        ) {
                            ActionButton(
                                text = strings.viewLog,
                                textStyle = layout.actionStyle,
                                minWidth = 0.dp,
                                modifier = Modifier.weight(1f),
                                focusRequester = logFocus,
                                onClick = onShowLog,
                            )
                            ActionButton(
                                text = strings.rename,
                                textStyle = layout.actionStyle,
                                minWidth = 0.dp,
                                modifier = Modifier.weight(1f),
                                focusRequester = renameFocus,
                                onClick = { model.beginRename() },
                            )
                        }
                    }
                }

                Spacer(modifier = Modifier.height(layout.spacingMedium))

                // 主页面默认只保留“开始接收”和常用入口，画质、语言、网卡等低频
                // 选项收进可展开区域。电视上不用每次进来都穿过一整页设置，焦点
                // 导航也更短；展开后仍按原顺序保留全部功能。
                ActionButton(
                    text = if (advancedOpen) strings.lessSettings else strings.moreSettings,
                    textStyle = layout.actionStyle,
                    minWidth = 0.dp,
                    modifier = Modifier.widthIn(min = 260.dp),
                    focusRequester = settingsFocus,
                    selected = advancedOpen,
                    onClick = { advancedOpen = !advancedOpen },
                )

                if (advancedOpen) {
                    Spacer(modifier = Modifier.height(layout.spacingLoose))

                    QualityRow(model = model, layout = layout)

                    Spacer(modifier = Modifier.height(layout.spacingLoose))

                    LanguageRow(model = model, layout = layout)

                    Spacer(modifier = Modifier.height(layout.spacingLoose))

                    BroadcastRow(model = model, layout = layout)
                }

                Spacer(modifier = Modifier.height(layout.spacingLoose))

                // 连接说明。用户的典型困惑是「手机搜不到电视」，多半是没连同一个
                // Wi-Fi 或路由器开了 AP 隔离（文档 6.3），所以把提示写在待机页上。
                //
                // widthIn 限制最大宽度：4K 电视上如果让文字铺满 1920dp，
                // 每行会太长而不便阅读。
                // 提示文案按输入方式区分：用户拿遥控器时提醒他按确定键，
                // 拿手机时提醒他点击。写反了会让人对着屏幕按半天。
                val hintLead = if (inputMode == InputMode.Remote) {
                    strings.hintRemoteLead
                } else {
                    strings.hintTouchLead
                }

                Box(
                    modifier = Modifier
                        .fillMaxWidth()
                        .widthIn(max = 900.dp)
                        .clip(RoundedCornerShape(16.dp))
                        .background(MaterialTheme.colorScheme.surfaceVariant)
                        .padding(horizontal = 24.dp, vertical = 18.dp),
                ) {
                    Text(
                        text = hintLead + strings.hintHowToCast,
                        style = layout.bodyStyle,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        textAlign = TextAlign.Center,
                        modifier = Modifier.fillMaxWidth(),
                    )
                }

                // 本机地址：手机搜不到设备时，DLNA 类 App 可以直接手填这个地址，
                // 也是判断「电视到底在哪张网卡上」最快的办法（文档 2.3 要求显示）。
                if (advancedOpen && model.localAddresses.isNotEmpty()) {
                    Spacer(modifier = Modifier.height(layout.spacingTight))

                    Text(
                        text = LocalStrings.current.localAddressLabel +
                            model.localAddresses.replace("\n", "  "),
                        style = layout.bodyStyle,
                        textAlign = TextAlign.Center,
                        modifier = Modifier.fillMaxWidth()
                    )
                }

                // 「手机搜不到这台电视」的兜底办法。
                //
                // 让**电视**开热点、手机连过来：两者就在同一张网里了，这条对付的是
                // 路由器开了 AP 隔离、或者压根没有可用路由器的情况（手机开热点是反
                // 过来的方向，那样得在电视上用遥控器敲 Wi-Fi 密码，更别说手机还要
                // 费电）。
                //
                // 只能跳设置页让用户自己按：真热点是系统签名级权限，应用开不了（见
                // EngineModel.openHotspotSettings）。所以这一块的重点是把「接下来该
                // 干什么」说清楚，而不是指望点了就自动开。
                if (advancedOpen) {
                    Spacer(modifier = Modifier.height(layout.spacingLoose))

                    Text(
                        text = LocalStrings.current.hotspotLead,
                        style = layout.bodyStyle,
                        textAlign = TextAlign.Center,
                        modifier = Modifier
                            .fillMaxWidth()
                            .widthIn(max = 900.dp)
                    )

                    Spacer(modifier = Modifier.height(layout.spacingTight))

                    ActionButton(
                        text = LocalStrings.current.openHotspotSettings,
                        textStyle = layout.actionStyle,
                        minWidth = 0.dp,
                        focusRequester = hotspotFocus,
                        onClick = { model.openHotspotSettings() },
                    )

                    // 三个候选页面都跳不到时的那句话。@Composable 里不留这个状态：它要
                    // 同时进日志，所以放在模型里（与其它失败提示一个去处）。
                    if (model.hotspotNote.isNotEmpty()) {
                        Spacer(modifier = Modifier.height(layout.spacingTight))

                        Text(
                            text = model.hotspotNote,
                            style = layout.bodyStyle,
                            color = Color(0xFFFF8A80),
                            textAlign = TextAlign.Center,
                            modifier = Modifier
                                .fillMaxWidth()
                                .widthIn(max = 900.dp)
                        )
                    }
                }
            }
        }
    }

    // 进来就给第一个按钮焦点，**不按设备类型判断**。
    //
    // 原来只在遥控器设备上抢焦点，理由是触屏上焦点框挂着不好看。但设备类型
    // 判断不可靠（电视盒子、手机接电视常常不上报 LEANBACK），一旦误判成触屏，
    // 就没有任何元素持有焦点 —— 方向键按下去没有任何反应，用户会以为应用卡死。
    LaunchedEffect(Unit) {
        buttonFocus.requestFocus()
    }
}
