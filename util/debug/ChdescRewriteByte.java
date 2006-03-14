import java.io.DataInput;
//import java.io.IOException;

public class ChdescRewriteByte extends Opcode
{
	private final int chdesc;
	
	public ChdescRewriteByte(int chdesc)
	{
		this.chdesc = chdesc;
	}
	
	public void applyTo(SystemState state)
	{
	}
	
	public String toString()
	{
		return "KDB_CHDESC_REWRITE_BYTE: chdesc = " + SystemState.hex(chdesc);
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_CHDESC_REWRITE_BYTE, "KDB_CHDESC_REWRITE_BYTE", ChdescRewriteByte.class);
		factory.addParameter("chdesc", 4);
		return factory;
	}
}
