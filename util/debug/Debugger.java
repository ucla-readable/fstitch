import java.io.*;
import java.util.HashMap;

public class Debugger
{
	private DataInput input;
	private HashMap modules;
	
	public Debugger(DataInput input) throws BadInputException, IOException
	{
		this.input = input;
		modules = new HashMap();
		
		addModule(new BdescModule(input));
		addModule(new ChdescAlterModule(input));
		
		short module = input.readShort();
		if(module != 0)
			throw new UnexpectedModuleException(module);
	}
	
	public void addModule(Module module)
	{
		//Short key = Short.valueOf(module.getModuleNumber());
		Short key = new Short(module.getModuleNumber());
		if(modules.containsKey(key))
			throw new RuntimeException("Duplicate module registered!");
		modules.put(key, module);
	}
	
	public static void main(String args[])
	{
		if(args.length != 1)
		{
			System.out.println("Usage: java Debugger <file>");
			return;
		}
		
		try {
			File file = new File(args[0]);
			InputStream input = new FileInputStream(file);
			DataInput data = new DataInputStream(input);
			Debugger dbg = new Debugger(data);
		}
		catch(BadInputException e)
		{
			System.out.println(e);
		}
		catch(EOFException e)
		{
			System.out.println("EOF!");
		}
		catch(IOException e)
		{
			System.out.println(e);
		}
	}
}
