import java.io.*;
import java.util.HashMap;
import java.util.Vector;

public class Debugger extends OpcodeFactory
{
	private HashMap modules;
	private Vector opcodes;
	
	public Debugger(DataInput input) throws BadInputException, IOException
	{
		super(input);
		modules = new HashMap();
		opcodes = new Vector();
		
		addModule(new BdescModule(input));
		addModule(new ChdescAlterModule(input));
		addModule(new ChdescInfoModule(input));
		
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
	
	public Opcode readOpcode() throws UnexpectedOpcodeException, IOException
	{
		throw new EOFException("Fake EOF");
	}
	
	public void readOpcodes() throws UnexpectedOpcodeException, IOException
	{
		try {
			for(;;)
				opcodes.add(readOpcode());
		}
		catch(EOFException e)
		{
			/* this is OK, we expect the end of the file sooner or later */
		}
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
			
			System.out.print("Reading debug signature... ");
			Debugger dbg = new Debugger(data);
			System.out.println("OK!");
			
			System.out.print("Reading debugging output... ");
			dbg.readOpcodes();
			System.out.println("OK!");
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
