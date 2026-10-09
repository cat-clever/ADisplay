// ADisplay —— 改设备名称
//
// 电视上没有键盘，所以改名的入口必须显式给出来（待机页上的「修改名称」）。
// 好消息是 Android TV 在聚焦输入框时会自己弹出系统输入法，遥控器就能操作 ——
// 所以这里只需要一个普通的输入框，不必自己做软键盘。
//
// 高度用 layout.editorHeight，**不跟日志区共用**。早先图省事复用了日志区的高度，
// 结果标题、输入框、两个按钮加起来比那几行等宽小字高得多，放不下：按钮那一行
// 被挤到屏幕外。遥控器还能靠焦点选中那个看不见的按钮再按确定，触屏就完全点不到
// —— 表现是「名字改完没法保存」。输入区该多大就多大。

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
import androidx.tv.material3.MaterialTheme
import androidx.tv.material3.Text
import com.adisplay.tv.EngineModel

@Composable
fun NameEditor(model: EngineModel, layout: StandbyLayout) {
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
        modifier = Modifier
            .fillMaxWidth()
            .height(layout.editorHeight)
            .clip(RoundedCornerShape(8.dp))
            .background(MaterialTheme.colorScheme.surfaceVariant)
            .border(1.dp, Color.White.copy(alpha = 0.18f), RoundedCornerShape(8.dp))
            .padding(horizontal = 18.dp, vertical = 12.dp),
        verticalArrangement = Arrangement.Center,
    ) {
        // 标题与校验提示同一行，而且提示限一行。
        //
        // 这样这块的高度是**恒定**的：提示另起一行、或者折成两行的话，名称一不
        // 合法整块就长高，把下面的按钮又挤出屏幕 —— 而名称不合法正是用户最需要
        // 看到按钮的时候。
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
                .clip(RoundedCornerShape(4.dp))
                .background(Color.White.copy(alpha = 0.08f))
                .border(1.dp, Color.White.copy(alpha = 0.25f), RoundedCornerShape(4.dp))
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
            // 比这块地方还宽，「取消」会被切掉一半（同 StandbyScreen 那三个按钮）。
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
