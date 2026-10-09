// ADisplay —— 改设备名称
//
// 电视上没有键盘，所以改名的入口必须显式给出来（待机页上的「修改名称」）。
// 好消息是 Android TV 在聚焦输入框时会自己弹出系统输入法，遥控器就能操作 ——
// 所以这里只需要一个普通的输入框，不必自己做软键盘。
//
// 尺寸刻意与日志区一致：这块地方是**借用**日志区的位置（见 StandbyScreen），
// 借用而不新开一块，是因为电视的布局是算着屏幕高度放的，多长出来会把上面的
// 按钮挤出可视区。

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

    Column(
        modifier = Modifier
            .fillMaxWidth()
            .height(layout.logHeight)
            .clip(RoundedCornerShape(8.dp))
            .background(MaterialTheme.colorScheme.surfaceVariant)
            .border(1.dp, Color.White.copy(alpha = 0.18f), RoundedCornerShape(8.dp))
            .padding(horizontal = 18.dp, vertical = 14.dp),
        verticalArrangement = Arrangement.Center,
    ) {
        Text(
            text = "设备名称",
            style = MaterialTheme.typography.titleMedium,
            color = Color.White.copy(alpha = 0.9f),
        )

        Spacer(modifier = Modifier.height(10.dp))

        BasicTextField(
            value = draft,
            onValueChange = { model.updateNameDraft(it) },
            singleLine = true,
            textStyle = layout.actionStyle.copy(color = Color.White),
            cursorBrush = SolidColor(Color.White),
            modifier = Modifier
                .fillMaxWidth()
                .clip(RoundedCornerShape(4.dp))
                .background(Color.White.copy(alpha = 0.08f))
                .border(1.dp, Color.White.copy(alpha = 0.25f), RoundedCornerShape(4.dp))
                .padding(horizontal = 12.dp, vertical = 10.dp)
                .focusRequester(fieldFocus),
        )

        // 输入时就给提示，而不是等按了保存才报错 —— 遥控器上打字很慢，
        // 越早说越好。
        if (problem.isNotEmpty()) {
            Spacer(modifier = Modifier.height(8.dp))
            Text(
                text = problem,
                style = MaterialTheme.typography.bodySmall,
                color = Color(0xFFFF8A80),
            )
        }

        Spacer(modifier = Modifier.height(12.dp))

        Row(
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.Center,
            modifier = Modifier.fillMaxWidth(),
        ) {
            ActionButton(
                text = "保存",
                textStyle = layout.actionStyle,
                minWidth = layout.buttonMinWidth,
                focusRequester = saveFocus,
                // 名称不合法时压暗：一眼看出还不能保存。
                enabled = draft.isNotEmpty() && problem.isEmpty(),
                onClick = { model.applyRename() },
            )

            Spacer(modifier = Modifier.width(16.dp))

            ActionButton(
                text = "取消",
                textStyle = layout.actionStyle,
                minWidth = layout.buttonMinWidth,
                focusRequester = cancelFocus,
                onClick = { model.cancelRename() },
            )
        }
    }
}
