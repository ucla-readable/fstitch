//import java.util.*;
//import java.io.*;
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
		return "Start GUI control panel, optionally rendering to PostScript.";
	}
	
	public Object runCommand(String args[], Object data, final CommandInterpreter interpreter) throws CommandException
	{
		final Debugger dbg = (Debugger) data;
		final GuiCommand shell = this;
		final String view = (args.length > 0) ? "ps " + args[0] : "view";
		final String title = "Debugger GUI" + ((args.length > 0) ? ": " + args[0] : "");
		if(dbg != null)
		{
			Runnable runnable = new Runnable()
			{
				public void run()
				{
					final JFrame frame = new JFrame(title);
					Container pane = frame.getContentPane();
					JButton button;
					
					frame.setDefaultCloseOperation(WindowConstants.DISPOSE_ON_CLOSE);
					frame.addWindowListener(new WindowAdapter()
					{
						public void windowClosed(WindowEvent e)
						{
							synchronized(shell)
							{
								shell.notify();
							}
						}
						/* this has the annoying side effect of always keeping
						 * it on top, not just bringing it back after a button
						 * has been pressed and another window shows up */
						/*public void windowDeactivated(WindowEvent e)
						{
							frame.toFront();
						}*/
					});
					
					ActionListener listener = new ActionListener()
					{
						public void actionPerformed(ActionEvent e)
						{
							try
							{
								String command = e.getActionCommand();
								interpreter.runCommandLine(command, dbg, false);
								if(!"view new".equals(command) && !view.equals(command))
									interpreter.runCommandLine(view, dbg, false);
								
								/* even though we're already in the event thread,
								 * we need this to happen after the invokeLater()
								 * that results from the view command above */
								SwingUtilities.invokeLater(new Runnable()
								{
									public void run()
									{
										frame.toFront();
									}
								});
							}
							catch(CommandException ex)
							{
							}
						}
					};
					
					pane.setLayout(new GridLayout(1, 5));
					
					button = new JButton("Start");
					button.setActionCommand("reset");
					button.addActionListener(listener);
					pane.add(button);
					
					button = new JButton(new ArrowIcon(SwingConstants.LEFT));
					button.setActionCommand("step -1");
					button.addActionListener(listener);
					pane.add(button);
					
					if("view".equals(view))
					{
						button = new JButton("New");
						button.setActionCommand("view new");
					}
					else
					{
						button = new JButton("PS");
						button.setActionCommand(view);
					}
					button.addActionListener(listener);
					pane.add(button);
					
					button = new JButton(new ArrowIcon(SwingConstants.RIGHT));
					button.setActionCommand("step");
					button.addActionListener(listener);
					pane.add(button);
					
					button = new JButton("End");
					button.setActionCommand("run");
					button.addActionListener(listener);
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
