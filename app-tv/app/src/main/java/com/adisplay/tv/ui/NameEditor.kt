// ADisplay —— 改设备名称（抽屉里的内容）
//
// 电视上没有键盘，所以改名的入口必须显式给出来（待机页上的「修改名称」）。
// Android TV 在聚焦输入框时会自己弹出系统输入法，遥控器就能操作 —— 所以这里
// 只需要一个普通的输入框，不必自己做软键盘。
//
// 它装在抽屉里（见 StandbyScreen）：竖屏从下面抽、横屏从侧边抽。以前它借的是
// 待机页底部日志区的位置**和高度**，而输入区里要放标题、输入框、两个按钮，
// 比那几行等宽小字高得多 —— 按钮那一行被挤到屏幕外，遥控器还能靠焦点选中那个
// 看不见的按钮，触屏就完全点不到。抽屉按内容取尺寸，没有这个问题。
//
// 这一层只是内容：面板的底色、圆角、内边距都由 Drawer 给。

package com.adisplay.tv.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.tv.material3.Text
import com.adisplay.tv.EngineModel

@Composable
fun NameEditor(model: EngineModel, layout: StandbyLayout, modifier: Modifier = Modifier) {
    val draft = model.nameDraft ?: return
    val problem = model.nameDraftProblem()

    val fieldFocus = remember { FocusRequester() }
    val saveFocus = remember { FocusRequester() }
    val cancelFocus = remember { FocusRequester() }

    // 一进来就把焦点放进输入框：这样系统输入法直接弹出来，用户少按一次。
    LaunchedEffect(Unit) {
        fieldFocus.requestFocus()
    }

    val canSave = draft.isNotEmpty() && problem.isEmpty()

    Column(
        modifier = modifier.fillMaxWidth(),
        verticalArrangement = Arrangement.Center,
    ) {
        // 标题与校验提示同一行，而且提示限一行 —— 让这块高度恒定，提示不会把
        // 下面的按钮顶走，而名称不合法正是用户最需要看到按钮的时候。
        Row(
            verticalAlignment = Alignment.CenterVertically,
            modifier = Modifier.fillMaxWidth(),
        ) {
            Text(
                text = "设备名称",
                style = layout.bodyStyle,
                color = Color.White.copy(alpha = 0.9f),
            )

            Spacer(modifier = Modifier.weight(1f))

            if (problem.isNotEmpty()) {
                Text(
                    text = problem,
                    style = layout.bodyStyle,
                    color = Color(0xFFFF8A80),
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
        }

        Spacer(modifier = Modifier.height(8.dp))

        BasicTextField(
            value = draft,
            onValueChange = { model.updateNameDraft(it) },
            singleLine = true,
            textStyle = layout.actionStyle.copy(color = Color.White),
            cursorBrush = SolidColor(Color.White),
            // 触屏上打完字直接按键盘的「完成」就保存，不必再去够下面的按钮 ——
            // 软键盘弹起来占掉半屏，够按钮这件事本来就别扭。
            keyboardOptions = KeyboardOptions(imeAction = ImeAction.Done),
            keyboardActions = KeyboardActions(onDone = {
                if (canSave) {
                    model.applyRename()
                }
            }),
            modifier = Modifier
                .fillMaxWidth()
                // 输入框自己的底色与描边。抽屉给了面板的底，但输入框仍要一眼
                // 看出「这里能打字」。
                .clip(RoundedCornerShape(6.dp))
                .background(Color.White.copy(alpha = 0.10f))
                .border(1.dp, Color.White.copy(alpha = 0.25f), RoundedCornerShape(6.dp))
                .padding(horizontal = 12.dp, vertical = 10.dp)
                .focusRequester(fieldFocus),
        )

        Spacer(modifier = Modifier.height(10.dp))

        Row(
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.Center,
            modifier = Modifier.fillMaxWidth(),
        ) {
            // 两个按钮用 weight 等分，不用固定宽度：固定宽度在手机竖屏上加起来
            // 比这块地方还宽，「取消」会被切掉一半（同待机页那三个按钮）。
            ActionButton(
                text = "保存",
                textStyle = layout.actionStyle,
                minWidth = 0.dp,
                modifier = Modifier.weight(1f),
                focusRequester = saveFocus,
                // 名称不合法时压暗：一眼看出还不能保存。
                enabled = canSave,
                onClick = { model.applyRename() },
            )

            Spacer(modifier = Modifier.width(16.dp))

            ActionButton(
                text = "取消",
                textStyle = layout.actionStyle,
                minWidth = 0.dp,
                modifier = Modifier.weight(1f),
                focusRequester = cancelFocus,
                onClick = { model.cancelRename() },
            )
        }
    }
}
