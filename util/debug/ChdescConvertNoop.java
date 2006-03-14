import java.io.DataInput;
//import java.io.IOException;

public class ChdescConvertNoop extends Opcode
{
	private final int chdesc;
	
	public ChdescConvertNoop(int chdesc)
	{
		this.chdesc = chdesc;
	}
	
	public void applyTo(SystemState state)
	{
		Chdesc chdesc = state.lookupChdesc(this.chdesc);
		if(chdesc != null)
			chdesc.changeToNoop();
	}
	
	public String toString()
	{
		return "KDB_CHDESC_CONVERT_NOOP: chdesc = " + SystemState.hex(chdesc);
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_CHDESC_CONVERT_NOOP, "KDB_CHDESC_CONVERT_NOOP", ChdescConvertNoop.class);
		factory.addParameter("chdesc", 4);
		return factory;
	}
}
