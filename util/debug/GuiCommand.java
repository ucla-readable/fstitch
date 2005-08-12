import java.util.*;
import java.io.*;
import javax.swing.*;
import java.awt.*;
import java.awt.event.*;

import command.*;

public class GuiCommand implements Command
{
	public String getName()
	{
		return "gui";
	}
	
	public String getHelp()
	{
		return "Start GUI control panel.";
	}
	
	public Object runCommand(String args[], Object data, final CommandInterpreter interpreter) throws CommandException
	{
		final Debugger dbg = (Debugger) data;
		final GuiCommand shell = this;
		if(dbg != null)
		{
			final WindowListener listenerW = new WindowAdapter()
			{
				public void windowClosed(WindowEvent e)
				{
					synchronized(shell)
					{
						shell.notify();
					}
				}
			};
			final ActionListener listenerB = new ActionListener()
			{
				public void actionPerformed(ActionEvent e)
				{
					try
					{
						interpreter.runCommandLine(e.getActionCommand(), dbg, false);
						interpreter.runCommandLine("view", dbg, false);
					}
					catch(CommandException ex)
					{
					}
				}
			};
			Runnable runnable = new Runnable()
			{
				public void run()
				{
					JFrame frame = new JFrame("Debugger GUI");
					Container pane = frame.getContentPane();
					JButton button;
					
					frame.setDefaultCloseOperation(WindowConstants.DISPOSE_ON_CLOSE);
					frame.addWindowListener(listenerW);
					
					pane.setLayout(new GridLayout(1, 5));
					
					button = new JButton("Start");
					button.setActionCommand("reset");
					button.addActionListener(listenerB);
					pane.add(button);
					
					button = new JButton(new ArrowIcon(SwingConstants.LEFT));
					button.setActionCommand("step -1");
					button.addActionListener(listenerB);
					pane.add(button);
					
					button = new JButton("View");
					button.setActionCommand("");
					button.addActionListener(listenerB);
					pane.add(button);
					
					button = new JButton(new ArrowIcon(SwingConstants.RIGHT));
					button.setActionCommand("step");
					button.addActionListener(listenerB);
					pane.add(button);
					
					button = new JButton("End");
					button.setActionCommand("run");
					button.addActionListener(listenerB);
					pane.add(button);
					
					frame.pack();
					frame.setVisible(true);
				}
			};
			System.out.println("Debugger GUI started. Close the GUI window to resume the command prompt.");
			SwingUtilities.invokeLater(runnable);
			for(;;)
				synchronized(this)
				{
					try {
						wait();
						break;
					}
					catch(InterruptedException e)
					{
					}
				}
		}
		else
			System.out.println("Need a file to debug.");
		return data;
	}
}
